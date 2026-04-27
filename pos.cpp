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

[[nodiscard]] static std::string genTxnId() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  std::mt19937 g{std::random_device{}()};
  std::ostringstream oss;
  oss << "pos-txn-" << ms << "-"
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
  try {
    if (!data.contains("card"))
      return err("ERR_MISSING_CARD");

    const json &card = data["card"];
    std::string txnId = genTxnId();
    std::string clientId = data.value("clientTxnId", txnId);
    std::string merchantId = data.value("merchantId", "");
    std::string terminalId = data.value("terminalId", "");
    std::string location = data.value("location", "");
    double amount = data.value("amount", 0.0);
    double fee = data.value("fee", 0.0);

    if (amount <= 0)
      return err("ERR_INVALID_AMOUNT", "Amount must be > 0");
    if (amount > POS_MAX_SINGLE)
      return err("ERR_SINGLE_LIMIT", "Exceeds POS single limit");

    // Decrypt PAN (always decrypt — PAN is always sent encrypted)
    std::string pan, expiry = card.value("expiry", ""),
                     cvv = card.value("cvv", "");
    if (card.contains("pan")) {
      try {
        pan = PANEncryptionService::getInstance().decryptPAN(
            card["pan"].get<std::string>());
      } catch (const std::exception &e) {
        return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
      }
    } else {
      return err("ERR_MISSING_PAN", "Card PAN is required");
    }

    // PIN verification (optional — tap payments may not have PIN)
    if (card.contains("pin")) {
      if (!PINService::getInstance().verifyPIN(pan,
                                               card["pin"].get<std::string>()))
        return err("ERR_INVALID_PIN", "PIN verification failed");
    }

    auto cardRes = ctx.cards.select("account_number", "scheme", "status", "expiry", "cvv")
                       .where("pan=:p")
                       .bind("p", pan)
                       .execute();
    if (cardRes.count() == 0)
      return err("ERR_CARD_NOT_FOUND");

    Row r = cardRes.fetchOne();
    std::string accNo = r[0].get<std::string>();
    std::string scheme = r[1].isNull() ? "" : r[1].get<std::string>();
    std::string dbExpiry = r[3].get<std::string>();
    std::string dbCvv = r[4].isNull() ? "" : r[4].get<std::string>();

    if (!expiry.empty() && expiry != dbExpiry) {
      return err("ERR_INVALID_EXPIRY", "Wrong expiry date");
    }
    
    if (!cvv.empty() && cvv != dbCvv) {
      return err("ERR_INVALID_CVV", "Wrong CVV");
    }

    if (r[2].get<std::string>() != "ACTIVE")
      return err("ERR_CARD_INACTIVE");

    // ── Falcon ────────────────────────────────────────────────────────
    Falcon falcon(*ctx.sess);
    std::string fraudReason;
    if (falcon.checkFraud(accNo, amount, fraudReason, FalconChannel::POS)) {
      falcon.logFraud(txnId, clientId, "", "", accNo, amount, fraudReason);
      return {{"status", "DECLINED"}, {"message", fraudReason}};
    }

    AccountLockManager::ScopedLock accLock(AccountLockManager::getInstance(), accNo, TxnPriority::DEBIT);

    double balance = ctx.accounts.select("balance")
                         .where("account_number=:a")
                         .bind("a", accNo)
                         .execute()
                         .fetchOne()[0]
                         .get<double>();

    if (balance < amount + fee)
      return err("ERR_INSUFFICIENT_FUNDS");

    ctx.sess->startTransaction();
    try {
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

      ctx.sess->commit();

      try {
        ctx.cards.update()
            .set("last_transaction_time", mysqlx::expr("NOW()"))
            .where("pan=:p")
            .bind("p", pan)
            .execute();
      } catch (...) {
      }

      return {{"transactionId", txnId},
              {"status", "SUCCESS"},
              {"balanceAfter", balance - (amount + fee)},
              {"message", "POS purchase successful"}};
    } catch (...) {
      ctx.sess->rollback();
      return err("ERR_DB_FAILURE");
    }

  } catch (const std::exception &e) {
    return err("ERR_EXCEPTION", e.what());
  }
}

// ── REFUND
// ────────────────────────────────────────────────────────────────────
[[nodiscard]] static json refund(const json &data, POSContext &ctx) {
  try {
    std::string txnId = genTxnId();
    std::string clientId = data.value("clientTxnId", txnId);
    std::string origTxnId = data.value("origTransactionId", "");
    double amount = data.value("amount", 0.0);
    std::string merchantId = data.value("merchantId", "");
    std::string terminalId = data.value("terminalId", "");
    std::string location = data.value("location", "");

    if (origTxnId.empty())
      return err("ERR_INVALID_ORIG_TXN", "origTransactionId required");

    auto pRes = ctx.posTbl
                    .select("id", "amount", "account_number", "card_pan",
                            "refunded_amount", "flag", "message")
                    .where("transaction_id=:tid")
                    .bind("tid", origTxnId)
                    .execute();
    if (pRes.count() == 0)
      return err("ERR_PURCHASE_NOT_FOUND");

    Row p = pRes.fetchOne();
    auto id = p[0].get<int64_t>();
    double purchAmt = p[1].get<double>();
    std::string acc = p[2].get<std::string>();
    std::string pan = p[3].get<std::string>();
    std::string flag = p[5].get<std::string>();
    std::string msg = p[6].get<std::string>();

    if (msg != "POS purchase successful")
      return err("ERR_INVALID_REFUND_SOURCE");
    if (flag != "N")
      return err("ERR_ALREADY_REFUNDED");
    if (amount <= 0 || amount > purchAmt)
      return err("ERR_INVALID_REFUND_AMOUNT");

    AccountLockManager::ScopedLock accLock(AccountLockManager::getInstance(), acc, TxnPriority::CREDIT);

    double balance = ctx.accounts.select("balance")
                         .where("account_number=:a")
                         .bind("a", acc)
                         .execute()
                         .fetchOne()[0]
                         .get<double>();
    Row feeRow = ctx.posTbl.select("fee")
                     .where("id=:id")
                     .bind("id", id)
                     .execute()
                     .fetchOne();
    double fee = feeRow[0].isNull() ? 0.0 : feeRow[0].get<double>();

    if (amount <= fee)
      return err("ERR_AMOUNT_LESS_THAN_FEE");
    double netRefund = amount - fee;

    ctx.sess->startTransaction();
    try {
      ctx.accounts.update()
          .set("balance", balance + netRefund)
          .where("account_number=:a")
          .bind("a", acc)
          .execute();

      std::string newFlag = (amount == purchAmt) ? "RF" : "PR";
      ctx.sess
          ->sql(
              "UPDATE transaction_pos SET refunded_amount=?,flag=? WHERE id=?")
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
                      amount, 0.0, pan, "SUCCESS", "POS refund successful", id)
              .execute();

      ctx.master.insert("table_name", "reference_id", "status")
          .values("transaction_pos", ins.getAutoIncrementValue(), "SUCCESS")
          .execute();

      ctx.sess->commit();
      return {{"transactionId", txnId},
              {"status", "SUCCESS"},
              {"flag", newFlag},
              {"netRefund", netRefund},
              {"message", "POS refund successful"}};
    } catch (...) {
      ctx.sess->rollback();
      return err("ERR_DB_FAILURE");
    }

  } catch (const std::exception &e) {
    return err("ERR_EXCEPTION", e.what());
  }
}

// ── Dispatch table (OCP)
// ──────────────────────────────────────────────────────
using POSHandler = std::function<json(const json &, POSContext &)>;
static const std::unordered_map<std::string, POSHandler> POS_DISPATCH = {
    {"PURCHASE", purchase},
    {"REFUND", refund},
};

// ── Public entry point
// ────────────────────────────────────────────────────────
json processPOSTransaction(const json &data) {
  try {
    Database::ScopedConnection sc;
    POSContext ctx(sc.operator->());

    std::string txnType = data.value("transactionType", "");
    auto it = POS_DISPATCH.find(txnType);
    if (it == POS_DISPATCH.end())
      return err("ERR_INVALID_TYPE", "Unsupported POS type: " + txnType);

    auto future = std::async(std::launch::async,
                             [&]() -> json { return it->second(data, ctx); });

    if (future.wait_for(std::chrono::seconds(POS_TIMEOUT_SEC)) ==
        std::future_status::timeout)
      return {{"status", "DECLINED"}, {"errorCode", "ERR_TIMEOUT"}};

    return future.get();
  } catch (const std::exception &e) {
    return err("ERR_FATAL", e.what());
  }
}