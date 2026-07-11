/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * MOBILE TRANSACTION PROCESSING  v3.0  (C++20 · Thread-Safe · Falcon-Protected)
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ Falcon now uses FalconChannel::MOBILE for tighter per-channel detection
 *   ✅ std::string_view parameters throughout for zero-copy
 *   ✅ Hourly / daily limits as constexpr
 *   ✅ [[nodiscard]] on all returning functions
 *   ✅ std::optional<std::string> for debitAcc resolution
 *   ✅ Named error builder avoids repetitive JSON construction
 *   ✅ rand() replaced with <random>
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "mobile.h"
#include "AccountLockManager.h"
#include "Database.h"
#include "DatabaseQueries.h"
#include "falcon.h"
#include "panencrypted.h"
#include "pin.h"
#include "TransactionLogger.h"
#include "global_contant.h"

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mysqlx/xdevapi.h>
#include <optional>
#include <random>
#include <sstream>

using namespace mysqlx;
using json = nlohmann::json;

// ── Compile-time limits
// ───────────────────────────────────────────────────────
inline constexpr double MOB_SINGLE_LIMIT = 2'000.0;
inline constexpr double MOB_HOURLY_LIMIT = 2'000.0;
inline constexpr double MOB_DAILY_LIMIT = 10'000.0;
inline constexpr int MOB_TIMEOUT_SEC = 6;

// ── TxnId
// ─────────────────────────────────────────────────────────────────────

// ── Error helper
// ──────────────────────────────────────────────────────────────
[[nodiscard]] static json err(std::string_view code, std::string_view msg) {
  return {{"errorCode", std::string(code)}, {"message", std::string(msg)}};
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CORE
// ═══════════════════════════════════════════════════════════════════════════════
[[nodiscard]] static json processMobileCore(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processMobileCore",
                                               {{"transactionType", data.value("transactionType", "")}});
  Database::ScopedConnection sc;
  Session &sess = *sc;
  Schema db = sess.getSchema("bankingdb");

  try {
    std::string clientTxnId = data.value("clientTxnId", TransactionLogger::currentUuid());
    TransactionType txnType = stringToTransactionType(data.value("transactionType", ""));
    std::string deviceId = data.value("deviceId", "");
    std::string mobileNo = data.value("mobileNumber", "");
    double amount = data.value("amount", 0.0);
    double fee = data.value("fee", 0.0);

    std::string creditAcc =
        data["creditAccount"]["accountNumber"].get<std::string>();

    Table accounts = db.getTable("accounts");
    Table cards = db.getTable("cards");
    Table mobileTable = db.getTable("transaction_mobile");
    Table masterTable = db.getTable("transactions");

    // ── Resolve debit account ─────────────────────────────────────────
    std::optional<std::string> debitAccOpt;

    if (data.contains("pan") && data.contains("pin")) {
      std::string encPan = data["pan"].get<std::string>();
      std::string pin = data["pin"].get<std::string>();

      // Decrypt PAN (logic unchanged per client requirement)
      std::string pan;
      try {
        pan = PANEncryptionService::getInstance().decryptPAN(encPan);
      } catch (const std::exception &e) {
        // No transaction open yet — no rollback needed
        trace.fail("encrypted PAN decrypt failed", {{"error", e.what()}});
        return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
      }

      if (pan.length() != 16) {
        // No transaction open yet — no rollback needed
        trace.fail("invalid PAN length");
        return err("ERR_INVALID_PAN", "PAN must be 16 digits");
      }

      // PIN verify (logic unchanged per client requirement)
      if (!PINService::getInstance().verifyPIN(pan, pin)) {
        // No transaction open yet — no rollback needed
        trace.fail("PIN verification failed");
        return err("ERR_INVALID_PIN", "PIN verification failed");
      }

      auto accRes = cards.select("account_number")
                        .where("pan = :p AND card_priority = 'PRIMARY'")
                        .bind("p", pan)
                        .execute();
      if (accRes.count() == 0) {
        // No transaction open yet — no rollback needed
        trace.fail("primary card not found for PAN");
        return err("ERR_CARD_NOT_FOUND", "No PRIMARY card for PAN");
      }
      debitAccOpt = accRes.fetchOne()[0].get<std::string>();

    } else if (data.contains("debitAccount")) {
      debitAccOpt = data["debitAccount"]["accountNumber"].get<std::string>();
    }

    if (!debitAccOpt) {
      // No transaction open yet — no rollback needed
      trace.fail("debit account could not be determined");
      return err("ERR_DEBIT_ACCOUNT_REQUIRED",
                 "Could not determine debit account");
    }
    const std::string &debitAcc = *debitAccOpt;

    // ── Falcon fraud check ────────────────────────────────────────────
    Falcon falcon(sess);
    std::string fraudReason;
    if (falcon.checkFraud(debitAcc, amount, fraudReason,
                          FalconChannel::MOBILE, &data)) {
      // Use the correlation UUID so the fraud-decline record ties back to the request log.
      std::string txnId = data.value("_correlationUuid", TransactionLogger::currentUuid());
      falcon.logFraud(txnId, clientTxnId, deviceId, mobileNo, debitAcc, amount,
                      fraudReason);
      sess.commit();
      trace.fail("fraud declined mobile transaction", {{"reason", fraudReason}});
      return {{"transactionId", txnId},
              {"status", "DECLINED"},
              {"message", fraudReason}};
    }

    // ── Transaction type validation ───────────────────────────────────
    if (txnType != TransactionType::FUND_TRANSFER) {
      // No transaction open yet — no rollback needed
      trace.fail("unsupported mobile transaction type", {{"transactionType", transactionTypeToString(txnType)}});
      return err("ERR_INVALID_TYPE", "Only FUND_TRANSFER supported for MOBILE");
    }

    // ── Single-transaction limit ──────────────────────────────────────
    if (amount > MOB_SINGLE_LIMIT) {
      // No transaction open yet — no rollback needed
      trace.fail("single transaction limit exceeded",
                 {{"amount", std::to_string(amount)},
                  {"limit", std::to_string(MOB_SINGLE_LIMIT)}});
      return err("ERR_ONE_TIME_LIMIT", "Single transaction limit is " +
                                           std::to_string(MOB_SINGLE_LIMIT));
    }

    // ── Lock Accounts to prevent Race Conditions ──────────────────────
    std::unique_ptr<AccountLockManager::ScopedLock> lock1, lock2;
    if (debitAcc < creditAcc) {
      lock1 = std::make_unique<AccountLockManager::ScopedLock>(
          AccountLockManager::getInstance(), debitAcc, TxnPriority::DEBIT);
      lock2 = std::make_unique<AccountLockManager::ScopedLock>(
          AccountLockManager::getInstance(), creditAcc, TxnPriority::CREDIT);
    } else if (creditAcc < debitAcc) {
      lock1 = std::make_unique<AccountLockManager::ScopedLock>(
          AccountLockManager::getInstance(), creditAcc, TxnPriority::CREDIT);
      lock2 = std::make_unique<AccountLockManager::ScopedLock>(
          AccountLockManager::getInstance(), debitAcc, TxnPriority::DEBIT);
    } else {
      lock1 = std::make_unique<AccountLockManager::ScopedLock>(
          AccountLockManager::getInstance(), debitAcc, TxnPriority::DEBIT);
    }

    sess.startTransaction();

    double debitBal = 0.0;
    double creditBal = 0.0;

    try {
      // Lock accounts in database in deterministic alphabetical order to prevent database deadlocks
      if (debitAcc < creditAcc) {
        auto r1 = DatabaseQueries::getAccountBalance(sess, debitAcc, true);
        if (!r1) {
          sess.rollback();
          return err("ERR_DEBIT_NOT_FOUND", "Debit account not found");
        }
        debitBal = *r1;

        auto r2 = DatabaseQueries::getAccountBalance(sess, creditAcc, true);
        if (!r2) {
          sess.rollback();
          return err("ERR_CREDIT_NOT_FOUND", "Credit account not found");
        }
        creditBal = *r2;
      } else if (creditAcc < debitAcc) {
        auto r2 = DatabaseQueries::getAccountBalance(sess, creditAcc, true);
        if (!r2) {
          sess.rollback();
          return err("ERR_CREDIT_NOT_FOUND", "Credit account not found");
        }
        creditBal = *r2;

        auto r1 = DatabaseQueries::getAccountBalance(sess, debitAcc, true);
        if (!r1) {
          sess.rollback();
          return err("ERR_DEBIT_NOT_FOUND", "Debit account not found");
        }
        debitBal = *r1;
      } else {
        auto r1 = DatabaseQueries::getAccountBalance(sess, debitAcc, true);
        if (!r1) {
          sess.rollback();
          return err("ERR_DEBIT_NOT_FOUND", "Debit account not found");
        }
        debitBal = *r1;
        creditBal = debitBal;
      }
    } catch (const std::exception& e) {
      sess.rollback();
      trace.fail("failed to lock accounts", {{"error", e.what()}});
      return err("ERR_DB_FAILURE", e.what());
    }

    // ── Hourly limit ──────────────────────────────────────────────────
    double hourlyTotal = DatabaseQueries::getMobileHourlySpent(sess, debitAcc);
    if (hourlyTotal + amount > MOB_HOURLY_LIMIT) {
      sess.rollback();
      trace.fail("hourly limit exceeded",
                 {{"hourlyTotal", std::to_string(hourlyTotal)},
                  {"amount", std::to_string(amount)}});
      return json{{"errorCode", "ERR_HOURLY_LIMIT"},
                  {"message", "Hourly limit exceeded"},
                  {"remainingLimit", MOB_HOURLY_LIMIT - hourlyTotal}};
    }

    // ── Daily limit ───────────────────────────────────────────────────
    double dailyTotal = DatabaseQueries::getMobileDailySpent(sess, debitAcc);
    if (dailyTotal + amount > MOB_DAILY_LIMIT) {
      sess.rollback();
      trace.fail("daily limit exceeded",
                 {{"dailyTotal", std::to_string(dailyTotal)},
                  {"amount", std::to_string(amount)}});
      return err("ERR_DAILY_LIMIT", "Daily limit of " +
                                        std::to_string(MOB_DAILY_LIMIT) +
                                        " exceeded");
    }

    if (debitBal < amount + fee) {
      sess.rollback();
      trace.fail("insufficient balance",
                 {{"balance", std::to_string(debitBal)},
                  {"amount", std::to_string(amount)},
                  {"fee", std::to_string(fee)}});
      return err("ERR_INSUFFICIENT_FUNDS", "Insufficient balance");
    }

    // ── Apply transfer ────────────────────────────────────────────────
    DatabaseQueries::updateAccountBalance(sess, debitAcc, debitBal - amount - fee);
    DatabaseQueries::updateAccountBalance(sess, creditAcc, creditBal + amount);

    // Use the correlation UUID as the DB transaction_id — one ID for everything.
    std::string txnId = data.value("_correlationUuid", TransactionLogger::currentUuid());

    auto ins =
        mobileTable
            .insert("transaction_id", "client_txn_id", "device_id",
                    "mobile_number", "account_number", "amount", "fee",
                    "status", "message")
            .values(txnId, clientTxnId, deviceId, mobileNo, debitAcc, amount,
                    fee, "SUCCESS", "Mobile fund transfer successful")
            .execute();

    masterTable.insert("table_name", "reference_id", "status")
        .values("transaction_mobile", ins.getAutoIncrementValue(), "SUCCESS")
        .execute();

    sess.commit();

    // Update last_transaction_time (best-effort, outside transaction)
    try {
      cards.update()
          .set("last_transaction_time", mysqlx::expr("NOW()"))
          .where("account_number=:acc AND card_priority='PRIMARY'")
          .bind("acc", debitAcc)
          .execute();
    } catch (...) {
    }

    trace.success({{"transactionId", txnId},
                   {"debitAccount", debitAcc},
                   {"creditAccount", creditAcc}});
    return {{"transactionId", txnId},
            {"status", "SUCCESS"},
            {"balanceAfterDebit", debitBal - amount - fee},
            {"message", "Fund transfer successful"}};

  } catch (const std::exception &e) {
    try {
      sess.rollback();
    } catch (...) {
    }
    trace.fail("mobile core exception", {{"error", e.what()}});
    return err("ERR_EXCEPTION", e.what());
  }
}

// ── Public entry point
// ────────────────────────────────────────────────────────
json processMobileTransaction(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processMobileTransaction",
                                               {{"transactionType", data.value("transactionType", "")}});
  std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
  TransactionLogger::ScopedContext scope(uuid, "MOBILE");
  TransactionLogger::instance().logCurrent(
      "INFO", "channel_handler_scheduled",
      "Mobile transaction handler scheduled",
      {{"transactionType", data.value("transactionType", "")}});

  auto future = std::async(std::launch::async, [data, uuid]() {
    TransactionLogger::ScopedContext asyncScope(uuid, "MOBILE");
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_started",
        "Mobile transaction handler started",
        {{"transactionType", data.value("transactionType", "")}});
    return processMobileCore(data);
  });
  if (future.wait_for(std::chrono::seconds(MOB_TIMEOUT_SEC)) ==
      std::future_status::timeout) {
    TransactionLogger::instance().logCurrent(
        "WARN", "channel_handler_timeout",
        "Mobile transaction handler timeout",
        {{"timeoutSeconds", std::to_string(MOB_TIMEOUT_SEC)}});
    trace.fail("mobile transaction timeout");
    return {{"status", "DECLINED"},
            {"errorCode", "ERR_TIMEOUT"},
            {"message", "Mobile transaction timeout (>" +
                            std::to_string(MOB_TIMEOUT_SEC) + "s)"}};
  }
  json result = future.get();
  TransactionLogger::instance().logCurrent(
      "INFO", "channel_handler_finished",
      "Mobile transaction handler finished",
      {{"transactionId", result.value("transactionId", "")},
       {"status", result.value("status", "")},
       {"errorCode", result.value("errorCode", "")}});
  if (result.contains("errorCode")) {
    trace.fail("mobile transaction failed",
               {{"errorCode", result["errorCode"].get<std::string>()}});
  } else {
    trace.success({{"transactionId", result.value("transactionId", "")},
                   {"status", result.value("status", "")}});
  }
  return result;
}
