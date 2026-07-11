/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * POS TRANSACTION PROCESSING  v3.0  (C++20 · Thread-Safe · Falcon-Protected)
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ Falcon uses FalconChannel::POS for tighter detection
 *   ✅ Context struct uses string_view-based helpers
 *   ✅ purchase / refund split into private lambdas registered in unordered_map
 *   ✅ constexpr limits for POS
 *   ✅ All DB errors produce structured JSON with errorCode
 *   ✅ [[nodiscard]] throughout
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "pos.h"
#include "Database.h"
#include "falcon.h"
#include "panencrypted.h"
#include "pin.h"
#include "AccountLockManager.h"
#include "TransactionLogger.h"
#include "accounting.h"
#include "currency_converter.h"
#include "global_contant.h"

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

inline constexpr double POS_MAX_SINGLE = 50'000.0;
inline constexpr int POS_TIMEOUT_SEC = 5;


[[nodiscard]] static json err(std::string_view code,
                              std::string_view msg = "") {
  json e{{"errorCode", std::string(code)}};
  if (!msg.empty())
    e["message"] = std::string(msg);
  return e;
}

// ── Context
// ───────────────────────────────────────────────────────────────────
struct POSContext {
  Session *sess;
  Schema db;
  Table cards, accounts, posTbl, master;

  explicit POSContext(Session *s)
      : sess(s), db(s->getSchema("bankingdb")), cards(db.getTable("cards")),
        accounts(db.getTable("accounts")),
        posTbl(db.getTable("transaction_pos")),
        master(db.getTable("transactions")) {}
};

// ── PURCHASE
// ──────────────────────────────────────────────────────────────────
[[nodiscard]] static json purchase(const json &data, POSContext &ctx) {
  TransactionLogger::ScopedFunctionTrace trace("purchase",
                                               {{"transactionType", transactionTypeToString(TransactionType::PURCHASE)}});
  try {
    if (!data.contains("card")) {
      trace.fail("missing card object");
      return err("ERR_MISSING_CARD");
    }

    const json &card = data["card"];
    // Use the correlation UUID as the DB transaction_id — one ID for everything.
    std::string txnId = data.value("_correlationUuid", TransactionLogger::currentUuid());
    std::string clientId = data.value("clientTxnId", txnId);
    std::string merchantId = data.value("merchantId", "");
    std::string terminalId = data.value("terminalId", "");
    std::string location = data.value("location", "");
    double amount = data.value("amount", 0.0);
    double fee = data.value("fee", 0.0);

    if (amount <= 0) {
      trace.fail("invalid purchase amount", {{"amount", std::to_string(amount)}});
      return err("ERR_INVALID_AMOUNT", "Amount must be > 0");
    }
    if (amount > POS_MAX_SINGLE) {
      trace.fail("POS single limit exceeded",
                 {{"amount", std::to_string(amount)},
                  {"limit", std::to_string(POS_MAX_SINGLE)}});
      return err("ERR_SINGLE_LIMIT", "Exceeds POS single limit");
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

    // PIN verification (optional — tap payments may not have PIN)
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
    if (falcon.checkFraud(accNo, amount, fraudReason, FalconChannel::POS,
                          &data)) {
      falcon.logFraud(txnId, clientId, terminalId, "", accNo, amount,
                      fraudReason);
      trace.fail("fraud declined POS purchase", {{"reason", fraudReason}});
      return {{"status", "DECLINED"}, {"message", fraudReason}};
    }

    // DCC Support: get account currency via JOIN (accounts has currency_id FK, not currency column)
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

    AccountLockManager::ScopedLock accLock(
        AccountLockManager::getInstance(), accNo, TxnPriority::DEBIT);

    ctx.sess->startTransaction();
    try {
      double balance = 0.0;
      auto balRes = ctx.sess->sql("SELECT balance FROM accounts WHERE account_number = ? FOR UPDATE")
                            .bind(accNo)
                            .execute();
      if (balRes.count() == 0) {
        ctx.sess->rollback();
        trace.fail("account not found");
        return err("ERR_ACCOUNT_NOT_FOUND");
      }
      balance = balRes.fetchOne()[0].get<double>();

      if (balance < amount + fee) {
        ctx.sess->rollback();
        trace.fail("insufficient balance",
                   {{"balance", std::to_string(balance)},
                    {"amount", std::to_string(amount)},
                    {"fee", std::to_string(fee)}});
        return err("ERR_INSUFFICIENT_FUNDS");
      }

      ctx.accounts.update()
          .set("balance", balance - (amount + fee))
          .where("account_number=:a")
          .bind("a", accNo)
          .execute();

      auto ins =
          ctx.posTbl
              .insert("transaction_id", "client_txn_id", "merchant_id",
                      "terminal_id", "location", "account_number", "amount",
                      "fee", "card_pan", "card_scheme", "status", "message",
                      "original_purchase_id", "refunded_amount", "flag")
              .values(txnId, clientId, merchantId, terminalId, location, accNo,
                      amount, fee, pan, scheme, "SUCCESS",
                      "POS purchase successful", nullptr, 0.0, "N")
              .execute();

      ctx.master.insert("table_name", "reference_id", "status")
          .values("transaction_pos", ins.getAutoIncrementValue(), "SUCCESS")
          .execute();

      Accounting::processPurchaseLedger(txnId, accNo, merchantId, amount, fee, fxMarkup, "POS Purchase", *ctx.sess);

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
              {"message", "POS purchase successful"}};
    } catch (...) {
      ctx.sess->rollback();
      trace.fail("database failure during POS purchase");
      return err("ERR_DB_FAILURE");
    }

  } catch (const std::exception &e) {
    trace.fail("POS purchase exception", {{"error", e.what()}});
    return err("ERR_EXCEPTION", e.what());
  }
}

// ── REFUND
// ────────────────────────────────────────────────────────────────────
[[nodiscard]] static json refund(const json &data, POSContext &ctx) {
  TransactionLogger::ScopedFunctionTrace trace("refund",
                                               {{"transactionType", transactionTypeToString(TransactionType::REFUND)}});
  try {
    // Use the correlation UUID as the DB transaction_id — one ID for everything.
    std::string txnId    = data.value("_correlationUuid", TransactionLogger::currentUuid());
    std::string clientId  = data.value("clientTxnId", txnId);
    std::string origTxnId = data.value("origTransactionId", "");
    double      amount    = data.value("amount", 0.0);
    std::string merchantId  = data.value("merchantId", "");
    std::string terminalId  = data.value("terminalId", "");
    std::string location    = data.value("location", "");

    if (origTxnId.empty()) {
      trace.fail("missing original transaction id");
      return err("ERR_INVALID_ORIG_TXN", "origTransactionId required");
    }
    if (merchantId.empty()) {
      trace.fail("missing merchantId in refund request");
      return err("ERR_MISSING_MERCHANT", "merchantId is required for refund");
    }

    std::string acc;
    auto accRes = ctx.sess->sql(
        "SELECT account_number FROM transaction_pos WHERE transaction_id = ?")
        .bind(origTxnId)
        .execute();
    if (accRes.count() > 0) {
      acc = accRes.fetchOne()[0].get<std::string>();
    }

    std::unique_ptr<AccountLockManager::ScopedLock> accLock;
    if (!acc.empty()) {
      accLock = std::make_unique<AccountLockManager::ScopedLock>(
          AccountLockManager::getInstance(), acc, TxnPriority::CREDIT);
    }

    // Start database transaction before executing the query, allowing us to
    // acquire a row lock via FOR UPDATE to prevent double-refund race conditions.
    ctx.sess->startTransaction();

    RowResult pRes;
    try {
      pRes = ctx.sess->sql(
          "SELECT id, amount, account_number, card_pan, refunded_amount, flag, message, merchant_id, fee "
          "FROM transaction_pos WHERE transaction_id = ? FOR UPDATE")
          .bind(origTxnId)
          .execute();
    } catch (const std::exception &e) {
      ctx.sess->rollback();
      trace.fail("failed to fetch and lock original transaction", {{"error", e.what()}});
      return err("ERR_DB_FAILURE", "Failed to lookup original transaction");
    }

    if (pRes.count() == 0) {
      ctx.sess->rollback();
      trace.fail("original purchase not found", {{"origTransactionId", origTxnId}});
      return err("ERR_PURCHASE_NOT_FOUND", "Original purchase transaction not found");
    }

    Row p = pRes.fetchOne();
    auto        id           = p[0].get<int64_t>();
    double      purchAmt     = p[1].get<double>();
    acc                      = p[2].get<std::string>();
    std::string origPan      = p[3].get<std::string>();   // PAN stored on original purchase
    double      refAmt       = p[4].isNull() ? 0.0 : p[4].get<double>();
    std::string flag         = p[5].get<std::string>();
    std::string msg          = p[6].get<std::string>();
    std::string origMerchant = p[7].isNull() ? "" : p[7].get<std::string>();
    // fee is in the original purchase row — no extra query needed
    double      origFee      = p[8].isNull() ? 0.0 : p[8].get<double>();

    // ── Guard 1: Must be a refundable purchase row ───────────────────────
    if (msg != "POS purchase successful") {
      ctx.sess->rollback();
      trace.fail("invalid refund source — original row is not a purchase");
      return err("ERR_INVALID_REFUND_SOURCE",
                 "origTransactionId does not reference a POS purchase");
    }

    // ── Guard 2: Merchant ownership check ────────────────────────────────
    if (origMerchant != merchantId) {
      ctx.sess->rollback();
      trace.fail("merchant mismatch — refund denied",
                 {{"requestMerchant",  merchantId},
                  {"originalMerchant", origMerchant}});
      return err("ERR_MERCHANT_MISMATCH",
                 "Refund can only be issued by the merchant who made the original sale");
    }

    // ── Guard 3: Card ownership check (when card is provided) ────────────
    if (data.contains("card") && data["card"].contains("pan")) {
      std::string refundPan;
      try {
        refundPan = PANEncryptionService::getInstance()
                        .decryptPAN(data["card"]["pan"].get<std::string>());
      } catch (const std::exception &e) {
        ctx.sess->rollback();
        trace.fail("encrypted PAN decrypt failed during refund card check",
                   {{"error", e.what()}});
        return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
      }

      if (refundPan != origPan) {
        ctx.sess->rollback();
        trace.fail("card PAN mismatch — refund denied",
                   {{"origTransactionId", origTxnId}});
        return err("ERR_CARD_MISMATCH",
                   "The card provided does not match the card used in the original purchase");
      }
    }

    // ── Guard 4: Already refunded ───────────────────────────────────────
    // Enforce that a POS transaction can only be refunded ONCE.
    if (flag != "N" || refAmt > 0) {
      ctx.sess->rollback();
      trace.fail("purchase already refunded", {{"flag", flag}, {"refundedAmount", std::to_string(refAmt)}});
      return err("ERR_ALREADY_REFUNDED", "This purchase has already been refunded");
    }

    // ── Guard 5: Refund amount validity ──────────────────────────────────
    if (amount <= 0 || amount > purchAmt) {
      ctx.sess->rollback();
      trace.fail("invalid refund amount",
                 {{"amount",          std::to_string(amount)},
                  {"purchaseAmount",   std::to_string(purchAmt)}});
      return err("ERR_INVALID_REFUND_AMOUNT",
                 "Refund amount must be greater than 0 and cannot exceed the purchase amount");
    }

    double balance = 0.0;
    try {
      RowResult balRes = ctx.sess->sql("SELECT balance FROM accounts WHERE account_number = ? FOR UPDATE")
                             .bind(acc)
                             .execute();
      if (balRes.count() == 0) {
        ctx.sess->rollback();
        trace.fail("account not found for refund", {{"accountNumber", acc}});
        return err("ERR_ACCOUNT_NOT_FOUND");
      }
      balance = balRes.fetchOne()[0].get<double>();
    } catch (const std::exception &e) {
      ctx.sess->rollback();
      trace.fail("failed to fetch/lock account balance", {{"error", e.what()}});
      return err("ERR_DB_FAILURE", "Database error fetching account balance");
    }

    // Use fee from the original purchase row — avoids a separate SELECT query
    double fee = origFee;
    if (amount <= fee) {
      ctx.sess->rollback();
      trace.fail("refund amount less than fee",
                 {{"amount", std::to_string(amount)}, {"fee", std::to_string(fee)}});
      return err("ERR_AMOUNT_LESS_THAN_FEE",
                 "Refund amount must be greater than the transaction fee");
    }
    double netRefund = amount - fee;

    try {
      ctx.accounts.update()
          .set("balance", balance + netRefund)
          .where("account_number=:a")
          .bind("a", acc)
          .execute();

      std::string newFlag = (amount == purchAmt) ? "RF" : "PR";
      ctx.sess
          ->sql("UPDATE transaction_pos SET refunded_amount=?,flag=? WHERE id=?")
          .bind(amount)
          .bind(newFlag)
          .bind(id)
          .execute();

      auto ins =
          ctx.posTbl
              .insert("transaction_id", "client_txn_id", "merchant_id",
                      "terminal_id", "location", "account_number", "amount",
                      "fee", "card_pan", "status", "message",
                      "original_purchase_id")
              .values(txnId, clientId, merchantId, terminalId, location, acc,
                      amount, 0.0, origPan, "SUCCESS", "POS refund successful", id)
              .execute();

      ctx.master.insert("table_name", "reference_id", "status")
          .values("transaction_pos", ins.getAutoIncrementValue(), "SUCCESS")
          .execute();

      Accounting::processRefundLedger(txnId, acc, merchantId, netRefund, "POS Refund", *ctx.sess);

      ctx.sess->commit();

      trace.success({{"transactionId",  txnId},
                     {"netRefund",       std::to_string(netRefund)},
                     {"originalMerchant", origMerchant},
                     {"flag",            newFlag}});
      return {{"transactionId",    txnId},
              {"status",           "SUCCESS"},
              {"flag",             newFlag},
              {"netRefund",        netRefund},
              {"accountNumber",    acc},
              {"message",          "POS refund successful"}};
    } catch (...) {
      ctx.sess->rollback();
      trace.fail("database failure during POS refund");
      return err("ERR_DB_FAILURE", "Database error during refund — transaction rolled back");
    }

  } catch (const std::exception &e) {
    trace.fail("POS refund exception", {{"error", e.what()}});
    return err("ERR_EXCEPTION", e.what());
  }
}

// ── Dispatch table (OCP)
// ──────────────────────────────────────────────────────
using POSHandler = std::function<json(const json &, POSContext &)>;
static const std::unordered_map<TransactionType, POSHandler> POS_DISPATCH = {
    {TransactionType::PURCHASE, purchase},
    {TransactionType::REFUND, refund},
};

// ── Public entry point
// ────────────────────────────────────────────────────────
json processPOSTransaction(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processPOSTransaction",
                                               {{"transactionType", data.value("transactionType", "")}});
  try {
    std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
    TransactionLogger::ScopedContext scope(uuid, "POS");
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_scheduled",
        "POS transaction handler scheduled",
        {{"transactionType", data.value("transactionType", "")}});

    // DB connection and dispatch are acquired/resolved inside the async lambda
    // to prevent use-after-free of stack-allocated objects.
    auto future = std::async(std::launch::async,
                             [data, uuid]() -> json {
                               TransactionLogger::ScopedContext asyncScope(uuid, "POS");
                               TransactionLogger::instance().logCurrent(
                                   "INFO", "channel_handler_started",
                                   "POS transaction handler started",
                                   {{"transactionType", data.value("transactionType", "")}});
                               // Acquire DB connection inside async thread to avoid
                               // capturing stack-allocated ctx by reference (use-after-free)
                               Database::ScopedConnection sc;
                               POSContext ctx(sc.operator->());
                               std::string txnTypeStr = data.value("transactionType", "");
                               TransactionType txnType = stringToTransactionType(txnTypeStr);
                               auto it2 = POS_DISPATCH.find(txnType);
                               if (it2 == POS_DISPATCH.end()) {
                                 return err("ERR_INVALID_TYPE", "Unsupported POS type: " + txnTypeStr);
                               }
                               return it2->second(data, ctx);
                             });

    if (future.wait_for(std::chrono::seconds(POS_TIMEOUT_SEC)) ==
        std::future_status::timeout) {
      TransactionLogger::instance().logCurrent(
          "WARN", "channel_handler_timeout",
          "POS transaction handler timeout",
          {{"timeoutSeconds", std::to_string(POS_TIMEOUT_SEC)}});
      trace.fail("POS transaction timeout");
      return {{"status", "DECLINED"}, {"errorCode", "ERR_TIMEOUT"}};
    }

    json result = future.get();
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_finished",
        "POS transaction handler finished",
        {{"transactionId", result.value("transactionId", "")},
         {"status", result.value("status", "")},
         {"errorCode", result.value("errorCode", "")}});
    if (result.contains("errorCode")) {
      trace.fail("POS transaction failed",
                 {{"errorCode", result["errorCode"].get<std::string>()}});
    } else {
      trace.success({{"transactionId", result.value("transactionId", "")},
                     {"status", result.value("status", "")}});
    }
    return result;
  } catch (const std::exception &e) {
    trace.fail("POS fatal exception", {{"error", e.what()}});
    return err("ERR_FATAL", e.what());
  }
}
