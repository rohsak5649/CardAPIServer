/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * RINGPAY CONTACTLESS PAYMENT  v3.0  (C++20 · Thread-Safe · Falcon-Protected)
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ Falcon fraud detection wired in (was MISSING in v2.x)
 *   ✅ Uses FalconChannel::RINGPAY
 *   ✅ Risk score thresholds as constexpr
 *   ✅ std::string_view params, [[nodiscard]]
 *   ✅ rand() replaced with std::mt19937
 *   ✅ Token expiry respects DB field (no silent re-use of expired token)
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "global_contant.h"
#include "Database.h"
#include "DatabaseQueries.h"
#include "falcon.h"
#include "panencrypted.h"
#include "pin.h"
#include "AccountLockManager.h"
#include "TransactionLogger.h"

#include <chrono>
#include <iostream>
#include <mysqlx/xdevapi.h>
#include <random>
#include <sstream>

using namespace mysqlx;
using json = nlohmann::json;

inline constexpr double RING_SINGLE_LIMIT = 2'000.0;
inline constexpr double RING_DAILY_LIMIT = 5'000.0;
inline constexpr double RING_WARNING_LEVEL = 4'000.0;
inline constexpr double RING_MERCHANT_LIMIT = 3'000.0;
inline constexpr int RING_RISK_BLOCK = 70;

[[nodiscard]] static std::string generateToken() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  std::mt19937 gen{std::random_device{}()};
  return "RING-" + std::to_string(ms) + "-" +
         std::to_string(std::uniform_int_distribution<>(100000, 999999)(gen));
}


[[nodiscard]] static json err(std::string_view code,
                              std::string_view msg = "") {
  json e{{"errorCode", std::string(code)}};
  if (!msg.empty())
    e["message"] = std::string(msg);
  return e;
}

json processRingPayTransaction(const json &data) {
  json response;
  TransactionLogger::ScopedFunctionTrace trace("processRingPayTransaction");
  std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
  TransactionLogger::ScopedContext scope(uuid, "RINGPAY");
  TransactionLogger::instance().logCurrent(
      "INFO", "channel_handler_started",
      "RingPay transaction handler started",
      {{"transactionType", data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE))}});

  Database::ScopedConnection sc;
  Session &sess = *sc;
  Schema db = sess.getSchema("bankingdb");

  try {
    std::string encPan = data["pan"].get<std::string>();
    std::string expiry = data.value("expiry", "");
    double amount = data["amount"].get<double>();
    double fee = data.value("fee", 0.0);
    std::string deviceId = data["deviceId"].get<std::string>();
    std::string ipAddr = data.value("ipAddress", "");
    std::string merchant = data["terminalId"].get<std::string>();

    // ── Decrypt PAN ───────────────────────────────────────────────────
    std::string pan;
    try {
      pan = PANEncryptionService::getInstance().decryptPAN(encPan);
    } catch (const std::exception &e) {
      trace.fail("encrypted PAN decrypt failed", {{"error", e.what()}});
      return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
    }

    // ── PIN verify (optional for contactless) ─────────────────────────
    if (data.contains("pin")) {
      std::string pin = data["pin"].get<std::string>();
      if (!PINService::getInstance().verifyPIN(pan, pin)) {
        trace.fail("PIN verification failed");
        return err("ERR_INVALID_PIN", "PIN verification failed");
      }
    }

    Table cards = db.getTable("cards");
    Table accounts = db.getTable("accounts");
    Table tokenTable = db.getTable("ringpay_tokens");
    Table ringTable = db.getTable("transaction_ringpay");
    Table masterTable = db.getTable("transactions");

    // ── Card validation ───────────────────────────────────────────────
    auto cardRes = cards.select("account_number", "status", "card_priority", "expiry", "cvv")
                       .where("pan=:p")
                       .bind("p", pan)
                       .execute();
    if (cardRes.count() == 0) {
      trace.fail("card not found");
      return err("ERR_INVALID_CARD", "Card not found");
    }

    Row cardRow = cardRes.fetchOne();
    std::string accNo = cardRow[0].get<std::string>();
    std::string cStatus = cardRow[1].get<std::string>();
    std::string cPrio = cardRow[2].get<std::string>();
    std::string dbExpiry = cardRow[3].get<std::string>();
    std::string dbCvv = cardRow[4].isNull() ? "" : cardRow[4].get<std::string>();

    if (!expiry.empty() && expiry != dbExpiry) {
      trace.fail("invalid expiry");
      return err("ERR_INVALID_EXPIRY", "Wrong expiry date");
    }

    if (data.contains("cvv")) {
      std::string reqCvv = data["cvv"].get<std::string>();
      if (!reqCvv.empty() && reqCvv != dbCvv) {
        trace.fail("invalid CVV");
        return err("ERR_INVALID_CVV", "Wrong CVV");
      }
    }

    if (cStatus != "ACTIVE" || cPrio != "PRIMARY") {
      trace.fail("card priority/status rule failed",
                 {{"cardStatus", cStatus}, {"cardPriority", cPrio}});
      return err("ERR_CARD_RULE",
                 "Only PRIMARY ACTIVE card allowed for RingPay");
    }

    // ── Account freeze check ─────────────────────────────────────────
    bool frozen = accounts.select("is_frozen")
                      .where("account_number=:a")
                      .bind("a", accNo)
                      .execute()
                      .fetchOne()[0]
                      .get<bool>();
    if (frozen) {
      trace.fail("account frozen", {{"accountNumber", accNo}});
      return err("ERR_ACCOUNT_FROZEN", "Account is frozen");
    }

    // ── Falcon fraud check (NEW in v3) ────────────────────────────────
    Falcon falcon(sess);
    std::string fraudReason;
    // Use the correlation UUID injected by the router as the DB transaction_id.
    // ring-txn-<ms> had ZERO randomness — any burst of requests in the same
    // millisecond produced identical IDs, guaranteed. UUID eliminates this.
    std::string txnId = data.value("_correlationUuid", TransactionLogger::currentUuid());
    if (falcon.checkFraud(accNo, amount, fraudReason, FalconChannel::RINGPAY,
                          &data)) {
      std::string clientId = data.value("clientTxnId", txnId);
      falcon.logFraud(txnId, clientId, deviceId, "", accNo, amount,
                      fraudReason);
      trace.fail("fraud declined RingPay transaction", {{"reason", fraudReason}});
      return {{"transactionId", txnId},
              {"status", "DECLINED"},
              {"message", fraudReason}};
    }

    // ── Token management ──────────────────────────────────────────────
    auto tokRes =
        tokenTable.select("token")
            .where(
                "account_number=:a AND status='ACTIVE' AND expires_at > NOW()")
            .bind("a", accNo)
            .execute();
    std::string token;
    if (tokRes.count() == 0) {
      token = generateToken();
      tokenTable.insert("token", "card_pan", "account_number")
          .values(token, pan, accNo)
          .execute();
    } else {
      token = tokRes.fetchOne()[0].get<std::string>();
    }

    // ── Limits ────────────────────────────────────────────────────────
    if (amount > RING_SINGLE_LIMIT) {
      trace.fail("RingPay single limit exceeded",
                 {{"amount", std::to_string(amount)},
                  {"limit", std::to_string(RING_SINGLE_LIMIT)}});
      return err("ERR_SINGLE_LIMIT", "Max " +
                                         std::to_string(RING_SINGLE_LIMIT) +
                                         " per RingPay txn");
    }

    double usedToday =
        ringTable.select("IFNULL(SUM(amount),0)")
            .where("account_number=:a AND DATE(created_at)=CURDATE() AND "
                   "status='SUCCESS'")
            .bind("a", accNo)
            .execute()
            .fetchOne()[0]
            .get<double>();

    if (usedToday + amount > RING_DAILY_LIMIT) {
      trace.fail("RingPay daily limit exceeded",
                 {{"usedToday", std::to_string(usedToday)},
                  {"amount", std::to_string(amount)}});
      return err("ERR_DAILY_LIMIT", "Daily RingPay limit exceeded");
    }

    if (usedToday + amount > RING_WARNING_LEVEL)
      response["warning"] = "Approaching daily RingPay limit";

    double merchantUsed = ringTable.select("IFNULL(SUM(amount),0)")
                              .where("account_number=:a AND merchant_id=:m AND "
                                     "DATE(created_at)=CURDATE()")
                              .bind("a", accNo)
                              .bind("m", merchant)
                              .execute()
                              .fetchOne()[0]
                              .get<double>();
    if (merchantUsed + amount > RING_MERCHANT_LIMIT) {
      trace.fail("RingPay merchant daily limit exceeded",
                 {{"merchantUsed", std::to_string(merchantUsed)},
                  {"amount", std::to_string(amount)}});
      return err("ERR_MERCHANT_LIMIT", "Merchant daily limit exceeded");
    }

    // ── Risk scoring ──────────────────────────────────────────────────
    int risk = 0;
    if (amount > 1500)
      risk += 40;
    if (usedToday > 3000)
      risk += 40;
    if (risk >= RING_RISK_BLOCK) {
      trace.fail("RingPay high risk score blocked", {{"riskScore", std::to_string(risk)}});
      return {{"status", "BLOCKED"},
              {"reason", "High risk score: " + std::to_string(risk)}};
    }

    // ── Balance and Transaction ───────────────────────────────────────
    AccountLockManager::ScopedLock accLock(
        AccountLockManager::getInstance(), accNo, TxnPriority::DEBIT);

    sess.startTransaction();
    try {
      auto currentBalOpt = DatabaseQueries::getAccountBalance(sess, accNo, true);
      if (!currentBalOpt) {
        sess.rollback();
        trace.fail("account not found", {{"accountNumber", accNo}});
        return err("ERR_ACCOUNT_NOT_FOUND", "Account not found");
      }
      double balance = *currentBalOpt;

      if (balance < amount + fee) {
        sess.rollback();
        trace.fail("insufficient balance",
                   {{"balance", std::to_string(balance)},
                    {"amount", std::to_string(amount)},
                    {"fee", std::to_string(fee)}});
        return err("ERR_INSUFFICIENT_FUNDS", "Insufficient balance");
      }

      double newBal = balance - (amount + fee);
      DatabaseQueries::updateAccountBalance(sess, accNo, newBal);

      auto ringRes = ringTable
                         .insert("transaction_id", "token", "account_number",
                                 "amount", "fee", "status", "message",
                                 "device_id", "ip_address", "merchant_id")
                         .values(txnId, token, accNo, amount, fee, "PROCESSING",
                                 "RingPay Processing", deviceId, ipAddr, merchant)
                         .execute();
      long childId = ringRes.getAutoIncrementValue();

      // ── Simulate random failure (10% chance) ─────────────────────────
      std::mt19937 g{std::random_device{}()};
      bool failed = (std::uniform_int_distribution<>(0, 9)(g) == 0);
      if (failed) {
        DatabaseQueries::updateAccountBalance(sess, accNo, balance);
        ringTable.update()
            .set("status", "FAILED")
            .set("reversal_status", "REVERSED")
            .where("transaction_id=:t")
            .bind("t", txnId)
            .execute();
        sess.commit();
        trace.fail("RingPay network failure auto reversed", {{"transactionId", txnId}});
        return {{"status", "FAILED"},
                {"message", "Network failure — auto reversed"}};
      }

      ringTable.update()
          .set("status", "SUCCESS")
          .where("transaction_id=:t")
          .bind("t", txnId)
          .execute();
      masterTable.insert("table_name", "reference_id", "status")
          .values("transaction_ringpay", childId, "SUCCESS")
          .execute();

      sess.commit();

      response["status"] = "SUCCESS";
      response["transactionId"] = txnId;
      response["token"] = token;
      response["balanceAfter"] = newBal;
      response["riskScore"] = risk;
      trace.success({{"transactionId", txnId},
                     {"riskScore", std::to_string(risk)},
                     {"balanceAfter", std::to_string(newBal)}});
      return response;
    } catch (...) {
      sess.rollback();
      throw;
    }

  } catch (const std::exception &e) {
    trace.fail("RingPay exception", {{"error", e.what()}});
    return err("ERR_EXCEPTION", e.what());
  }
}
