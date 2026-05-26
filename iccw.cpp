/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * ICCW TRANSACTION PROCESSING  v3.0  (C++20 · Thread-Safe · Falcon-Protected)
 * ─────────────────────────────────────────────────────────────────────────────
 * Conforms identically to POS purchase flow with limits:
 *   - Max single withdrawal: 4000
 *   - Multiple of 100
 */

#include "iccw.h"
#include "Database.h"
#include "falcon.h"
#include "panencrypted.h"
#include "pin.h"
#include "AccountLockManager.h"
#include "TransactionLogger.h"
#include "accounting.h"
#include "currency_converter.h"

#include <cmath>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <mysqlx/xdevapi.h>
#include <random>
#include <sstream>
#include <unordered_map>

using namespace mysqlx;
using json = nlohmann::json;

inline constexpr double ICCW_MAX_SINGLE = 4000.0;
inline constexpr int ICCW_TIMEOUT_SEC = 5;

[[nodiscard]] static std::string genTxnId() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  std::mt19937 g{std::random_device{}()};
  std::ostringstream oss;
  oss << "iccw-txn-" << ms << "-"
      << std::uniform_int_distribution<>(1000, 9999)(g);
  return oss.str();
}

[[nodiscard]] static json err(std::string_view code,
                              std::string_view msg = "") {
  json e{{"errorCode", std::string(code)}};
  if (!msg.empty())
    e["message"] = std::string(msg);
  return e;
}

// ── Context
// ───────────────────────────────────────────────────────────────────
struct ICCWContext {
  Session *sess;
  Schema db;
  Table cards, accounts, iccwTbl, master;

  explicit ICCWContext(Session *s)
      : sess(s), db(s->getSchema("bankingdb")), cards(db.getTable("cards")),
        accounts(db.getTable("accounts")),
        iccwTbl(db.getTable("transaction_iccw")),
        master(db.getTable("transactions")) {}
};

// ── WITHDRAWAL
// ──────────────────────────────────────────────────────────────────
[[nodiscard]] static json withdrawal(const json &data, ICCWContext &ctx) {
  TransactionLogger::ScopedFunctionTrace trace("withdrawal",
                                               {{"transactionType", "WITHDRAWAL"}});
  try {
    if (!data.contains("card")) {
      trace.fail("missing card object");
      return err("ERR_MISSING_CARD");
    }

    const json &card = data["card"];
    std::string txnId = genTxnId();
    std::string clientId = data.value("clientTxnId", txnId);
    std::string merchantId = data.value("merchantId", "");
    std::string terminalId = data.value("terminalId", "");
    std::string location = data.value("location", "");
    double amount = data.value("amount", 0.0);
    double fee = data.value("fee", 0.0);

    if (amount <= 0) {
      trace.fail("invalid withdrawal amount", {{"amount", std::to_string(amount)}});
      return err("ERR_INVALID_AMOUNT", "Amount must be > 0");
    }
    if (amount > ICCW_MAX_SINGLE) {
      trace.fail("ICCW single limit exceeded",
                 {{"amount", std::to_string(amount)},
                  {"limit", std::to_string(ICCW_MAX_SINGLE)}});
      return err("ERR_SINGLE_LIMIT", "Exceeds ICCW single limit");
    }
    if (std::fmod(amount, 100.0) != 0.0) {
      trace.fail("ICCW amount not a multiple of 100", {{"amount", std::to_string(amount)}});
      return err("ERR_INVALID_AMOUNT_MULTIPLE", "Amount must be a multiple of 100");
    }

    // Decrypt PAN (always decrypt — PAN is always sent encrypted)
    std::string pan, expiry = card.value("expiry", ""),
                     cvv = card.value("cvv", "");
    if (card.contains("pan")) {
      try {
        pan = PANEncryptionService::getInstance().decryptPAN(
            card["pan"].get<std::string>());
      } catch (const std::exception &e) {
        trace.fail("encrypted PAN decrypt failed", {{"error", e.what()}});
        return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
      }
    } else {
      trace.fail("missing PAN");
      return err("ERR_MISSING_PAN", "Card PAN is required");
    }

    // PIN verification (optional — tap/cardless payments may not have PIN)
    if (card.contains("pin")) {
      if (!PINService::getInstance().verifyPIN(pan,
                                               card["pin"].get<std::string>())) {
        trace.fail("PIN verification failed");
        return err("ERR_INVALID_PIN", "PIN verification failed");
      }
    }

    auto cardRes = ctx.cards.select("account_number", "scheme", "status", "expiry", "cvv")
                       .where("pan=:p")
                       .bind("p", pan)
                       .execute();
    if (cardRes.count() == 0) {
      trace.fail("card not found");
      return err("ERR_CARD_NOT_FOUND");
    }

    Row r = cardRes.fetchOne();
    std::string accNo = r[0].get<std::string>();
    std::string scheme = r[1].isNull() ? "" : r[1].get<std::string>();
    std::string dbExpiry = r[3].get<std::string>();
    std::string dbCvv = r[4].isNull() ? "" : r[4].get<std::string>();

    if (!expiry.empty() && expiry != dbExpiry) {
      trace.fail("invalid expiry");
      return err("ERR_INVALID_EXPIRY", "Wrong expiry date");
    }
    
    if (!cvv.empty() && cvv != dbCvv) {
      trace.fail("invalid CVV");
      return err("ERR_INVALID_CVV", "Wrong CVV");
    }

    if (r[2].get<std::string>() != "ACTIVE") {
      trace.fail("card inactive");
      return err("ERR_CARD_INACTIVE");
    }

    // ── Falcon ────────────────────────────────────────────────────────
    Falcon falcon(*ctx.sess);
    std::string fraudReason;
    if (falcon.checkFraud(accNo, amount, fraudReason, FalconChannel::ICCW,
                          &data)) {
      falcon.logFraud(txnId, clientId, terminalId, "", accNo, amount,
                      fraudReason);
      trace.fail("fraud declined ICCW purchase", {{"reason", fraudReason}});
      return {{"status", "DECLINED"}, {"message", fraudReason}};
    }

    // DCC Support: get account currency via JOIN
    std::string reqCurrency = data.value("currency", "AUD");
    double fxMarkup = 0.0;
    double fxRate = 1.0;
    auto currRow = ctx.sess->sql(
        "SELECT c.currency_code FROM accounts a "
        "JOIN currency c ON c.currency_id = a.currency_id "
        "WHERE a.account_number = ?")
        .bind(accNo)
        .execute()
        .fetchOne();
    std::string accCurrency = (currRow && !currRow[0].isNull()) ? currRow[0].get<std::string>() : "AUD";

    if (accCurrency != reqCurrency) {
        auto fxRes = CurrencyConverter::convertToBase(reqCurrency, amount, *ctx.sess);
        amount = fxRes.convertedAmount;
        fxMarkup = fxRes.bankMarkupAmount;
        fxRate = fxRes.fxRate;
        trace.success({{"convertedAmount", std::to_string(amount)}, {"fxRate", std::to_string(fxRate)}});
    }

    AccountLockManager::ScopedLock accLock(AccountLockManager::getInstance(), accNo, TxnPriority::DEBIT);

    double balance = ctx.accounts.select("balance")
                         .where("account_number=:a")
                         .bind("a", accNo)
                         .execute()
                         .fetchOne()[0]
                         .get<double>();

    if (balance < amount + fee) {
      trace.fail("insufficient balance",
                 {{"balance", std::to_string(balance)},
                  {"amount", std::to_string(amount)},
                  {"fee", std::to_string(fee)}});
      return err("ERR_INSUFFICIENT_FUNDS");
    }

    ctx.sess->startTransaction();
    try {
      ctx.accounts.update()
          .set("balance", balance - (amount + fee))
          .where("account_number=:a")
          .bind("a", accNo)
          .execute();

      auto ins =
          ctx.iccwTbl
              .insert("transaction_id", "client_txn_id", "merchant_id",
                      "terminal_id", "location", "account_number", "amount",
                      "fee", "card_pan", "card_scheme", "status", "message",
                      "original_purchase_id", "refunded_amount", "flag")
              .values(txnId, clientId, merchantId, terminalId, location, accNo,
                      amount, fee, pan, scheme, "SUCCESS",
                      "ICCW withdrawal successful", nullptr, 0.0, "N")
              .execute();

      ctx.master.insert("table_name", "reference_id", "status")
          .values("transaction_iccw", ins.getAutoIncrementValue(), "SUCCESS")
          .execute();

      Accounting::processPurchaseLedger(txnId, accNo, merchantId, amount, fee, fxMarkup, "ICCW Withdrawal", *ctx.sess);

      ctx.sess->commit();

      try {
        ctx.cards.update()
            .set("last_transaction_time", mysqlx::expr("NOW()"))
            .where("pan=:p")
            .bind("p", pan)
            .execute();
      } catch (...) {
      }

      trace.success({{"transactionId", txnId},
                     {"accountNumber", accNo}});
      return {{"transactionId", txnId},
              {"status", "SUCCESS"},
              {"balanceAfter", balance - (amount + fee)},
              {"message", "ICCW withdrawal successful"}};
    } catch (...) {
      ctx.sess->rollback();
      trace.fail("database failure during ICCW withdrawal");
      return err("ERR_DB_FAILURE");
    }

  } catch (const std::exception &e) {
    trace.fail("ICCW withdrawal exception", {{"error", e.what()}});
    return err("ERR_EXCEPTION", e.what());
  }
}

// ── Dispatch table
// ──────────────────────────────────────────────────────
using ICCWHandler = std::function<json(const json &, ICCWContext &)>;
static const std::unordered_map<std::string, ICCWHandler> ICCW_DISPATCH = {
    {"CASH_WITHDRAWAL", withdrawal},   // primary / canonical type
    {"WITHDRAWAL",      withdrawal},   // alias
    {"PURCHASE",        withdrawal},   // legacy alias
};

// ── Public entry point
// ────────────────────────────────────────────────────────
json processICCWTransaction(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processICCWTransaction",
                                               {{"transactionType", data.value("transactionType", "")}});
  try {
    std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
    TransactionLogger::ScopedContext scope(uuid, "ICCW");
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_scheduled",
        "ICCW transaction handler scheduled",
        {{"transactionType", data.value("transactionType", "")}});

    Database::ScopedConnection sc;
    ICCWContext ctx(sc.operator->());

    std::string txnType = data.value("transactionType", "WITHDRAWAL");
    auto it = ICCW_DISPATCH.find(txnType);
    if (it == ICCW_DISPATCH.end()) {
      trace.fail("unsupported ICCW transaction type", {{"transactionType", txnType}});
      return err("ERR_INVALID_TYPE", "Unsupported ICCW type: " + txnType);
    }

    auto future = std::async(std::launch::async,
                             [&, uuid]() -> json {
                               TransactionLogger::ScopedContext asyncScope(uuid, "ICCW");
                               TransactionLogger::instance().logCurrent(
                                   "INFO", "channel_handler_started",
                                   "ICCW transaction handler started",
                                   {{"transactionType", data.value("transactionType", "")}});
                               return it->second(data, ctx);
                             });

    if (future.wait_for(std::chrono::seconds(ICCW_TIMEOUT_SEC)) ==
        std::future_status::timeout) {
      TransactionLogger::instance().logCurrent(
          "WARN", "channel_handler_timeout",
          "ICCW transaction handler timeout",
          {{"timeoutSeconds", std::to_string(ICCW_TIMEOUT_SEC)}});
      trace.fail("ICCW transaction timeout");
      return {{"status", "DECLINED"}, {"errorCode", "ERR_TIMEOUT"}};
    }

    json result = future.get();
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_finished",
        "ICCW transaction handler finished",
        {{"transactionId", result.value("transactionId", "")},
         {"status", result.value("status", "")},
         {"errorCode", result.value("errorCode", "")}});
    if (result.contains("errorCode")) {
      trace.fail("ICCW transaction failed",
                 {{"errorCode", result["errorCode"].get<std::string>()}});
    } else {
      trace.success({{"transactionId", result.value("transactionId", "")},
                     {"status", result.value("status", "")}});
    }
    return result;
  } catch (const std::exception &e) {
    trace.fail("ICCW fatal exception", {{"error", e.what()}});
    return err("ERR_FATAL", e.what());
  }
}
