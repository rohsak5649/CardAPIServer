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
#include "Database.h"
#include "falcon.h"
#include "pin.h"
#include "panencrypted.h"

#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <future>
#include <optional>
#include <mysqlx/xdevapi.h>

using namespace mysqlx;
using json = nlohmann::json;

// ── Compile-time limits ───────────────────────────────────────────────────────
inline constexpr double MOB_SINGLE_LIMIT  = 2'000.0;
inline constexpr double MOB_HOURLY_LIMIT  = 2'000.0;
inline constexpr double MOB_DAILY_LIMIT   = 10'000.0;
inline constexpr int    MOB_TIMEOUT_SEC   = 6;

// ── TxnId ─────────────────────────────────────────────────────────────────────
[[nodiscard]] static std::string makeTxnId() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::mt19937 gen{std::random_device{}()};
    return "mobile-txn-" + std::to_string(ms) + "-" +
           std::to_string(std::uniform_int_distribution<>(1000,9999)(gen));
}

// ── Error helper ──────────────────────────────────────────────────────────────
[[nodiscard]] static json err(std::string_view code, std::string_view msg) {
    return {{"errorCode", std::string(code)}, {"message", std::string(msg)}};
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CORE
// ═══════════════════════════════════════════════════════════════════════════════
[[nodiscard]] static json processMobileCore(const json& data) {
    Database::ScopedConnection sc;
    Session& sess = *sc;
    Schema   db   = sess.getSchema("bankingdb");

    try {
        std::string clientTxnId = data.value("clientTxnId", makeTxnId());
        std::string txnType     = data.value("transactionType", "");
        std::string deviceId    = data.value("deviceId", "");
        std::string mobileNo    = data.value("mobileNumber", "");
        double amount           = data.value("amount", 0.0);
        double fee              = data.value("fee", 0.0);

        std::string creditAcc = data["creditAccount"]["accountNumber"].get<std::string>();

        Table accounts    = db.getTable("accounts");
        Table cards       = db.getTable("cards");
        Table mobileTable = db.getTable("transaction_mobile");
        Table masterTable = db.getTable("transactions");

        sess.startTransaction();

        // ── Resolve debit account ─────────────────────────────────────────
        std::optional<std::string> debitAccOpt;

        if (data.contains("pan") && data.contains("pin")) {
            std::string encPan = data["pan"].get<std::string>();
            std::string pin    = data["pin"].get<std::string>();

            // Decrypt PAN (logic unchanged per client requirement)
            std::string pan;
            try {
                pan = PANEncryptionService::getInstance().decryptPAN(encPan);
            } catch (const std::exception& e) {
                sess.rollback();
                return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
            }

            if (pan.length() != 16) {
                sess.rollback();
                return err("ERR_INVALID_PAN", "PAN must be 16 digits");
            }

            // PIN verify (logic unchanged per client requirement)
            if (!PINService::getInstance().verifyPIN(pan, pin)) {
                sess.rollback();
                return err("ERR_INVALID_PIN", "PIN verification failed");
            }

            auto accRes = cards.select("account_number")
                .where("pan = :p AND card_priority = 'PRIMARY'")
                .bind("p", pan).execute();
            if (accRes.count() == 0) {
                sess.rollback();
                return err("ERR_CARD_NOT_FOUND", "No PRIMARY card for PAN");
            }
            debitAccOpt = accRes.fetchOne()[0].get<std::string>();

        } else if (data.contains("debitAccount")) {
            debitAccOpt = data["debitAccount"]["accountNumber"].get<std::string>();
        }

        if (!debitAccOpt) {
            sess.rollback();
            return err("ERR_DEBIT_ACCOUNT_REQUIRED", "Could not determine debit account");
        }
        const std::string& debitAcc = *debitAccOpt;

        // ── Falcon fraud check ────────────────────────────────────────────
        Falcon falcon(sess);
        std::string fraudReason;
        if (falcon.checkFraud(debitAcc, amount, fraudReason, FalconChannel::MOBILE)) {
            std::string txnId = makeTxnId();
            falcon.logFraud(txnId, clientTxnId, deviceId, mobileNo, debitAcc, amount, fraudReason);
            sess.commit();
            return {
                {"transactionId", txnId},
                {"status",  "DECLINED"},
                {"message", fraudReason}
            };
        }

        // ── Transaction type validation ───────────────────────────────────
        if (txnType != "FUND_TRANSFER") {
            sess.rollback();
            return err("ERR_INVALID_TYPE", "Only FUND_TRANSFER supported for MOBILE");
        }

        // ── Single-transaction limit ──────────────────────────────────────
        if (amount > MOB_SINGLE_LIMIT) {
            sess.rollback();
            return err("ERR_ONE_TIME_LIMIT",
                       "Single transaction limit is " + std::to_string(MOB_SINGLE_LIMIT));
        }

        // ── Hourly limit ──────────────────────────────────────────────────
        auto hourlyRes = sess.sql(
            "SELECT IFNULL(SUM(amount),0) FROM transaction_mobile"
            " WHERE account_number=? AND status='SUCCESS'"
            " AND created_at >= NOW() - INTERVAL 1 HOUR")
            .bind(debitAcc).execute();
        double hourlyTotal = hourlyRes.fetchOne()[0].get<double>();
        if (hourlyTotal + amount > MOB_HOURLY_LIMIT) {
            sess.rollback();
            return json{
                {"errorCode",      "ERR_HOURLY_LIMIT"},
                {"message",        "Hourly limit exceeded"},
                {"remainingLimit", MOB_HOURLY_LIMIT - hourlyTotal}
            };
        }

        // ── Daily limit ───────────────────────────────────────────────────
        double dailyTotal = mobileTable.select("IFNULL(SUM(amount),0)")
            .where("account_number=:acc AND status='SUCCESS' AND DATE(created_at)=CURDATE()")
            .bind("acc", debitAcc).execute().fetchOne()[0].get<double>();
        if (dailyTotal + amount > MOB_DAILY_LIMIT) {
            sess.rollback();
            return err("ERR_DAILY_LIMIT", "Daily limit of " +
                       std::to_string(MOB_DAILY_LIMIT) + " exceeded");
        }

        // ── Balance check ─────────────────────────────────────────────────
        auto debitRow = accounts.select("balance")
            .where("account_number=:a").bind("a",debitAcc).execute();
        if (debitRow.count() == 0) {
            sess.rollback();
            return err("ERR_DEBIT_NOT_FOUND", "Debit account not found");
        }
        double debitBal = debitRow.fetchOne()[0].get<double>();

        auto creditRow = accounts.select("balance")
            .where("account_number=:a").bind("a",creditAcc).execute();
        if (creditRow.count() == 0) {
            sess.rollback();
            return err("ERR_CREDIT_NOT_FOUND", "Credit account not found");
        }
        double creditBal = creditRow.fetchOne()[0].get<double>();

        if (debitBal < amount + fee) {
            sess.rollback();
            return err("ERR_INSUFFICIENT_FUNDS", "Insufficient balance");
        }

        // ── Apply transfer ────────────────────────────────────────────────
        accounts.update().set("balance", debitBal - amount - fee)
            .where("account_number=:a").bind("a",debitAcc).execute();
        accounts.update().set("balance", creditBal + amount)
            .where("account_number=:a").bind("a",creditAcc).execute();

        std::string txnId = makeTxnId();
        auto ins = mobileTable.insert(
            "transaction_id","client_txn_id","device_id","mobile_number",
            "account_number","amount","fee","status","message")
            .values(txnId, clientTxnId, deviceId, mobileNo,
                    debitAcc, amount, fee, "SUCCESS", "Mobile fund transfer successful")
            .execute();

        masterTable.insert("table_name","reference_id","status")
            .values("transaction_mobile", ins.getAutoIncrementValue(), "SUCCESS").execute();

        sess.commit();

        // Update last_transaction_time (best-effort, outside transaction)
        try {
            cards.update().set("last_transaction_time", mysqlx::expr("NOW()"))
                .where("account_number=:acc AND card_priority='PRIMARY'")
                .bind("acc", debitAcc).execute();
        } catch (...) {}

        return {
            {"transactionId",     txnId},
            {"status",            "SUCCESS"},
            {"balanceAfterDebit", debitBal - amount - fee},
            {"message",           "Fund transfer successful"}
        };

    } catch (const std::exception& e) {
        try { sess.rollback(); } catch (...) {}
        return err("ERR_EXCEPTION", e.what());
    }
}

// ── Public entry point ────────────────────────────────────────────────────────
json processMobileTransaction(const json& data) {
    auto future = std::async(std::launch::async, processMobileCore, data);
    if (future.wait_for(std::chrono::seconds(MOB_TIMEOUT_SEC)) ==
        std::future_status::timeout) {
        return {
            {"status",    "DECLINED"},
            {"errorCode", "ERR_TIMEOUT"},
            {"message",   "Mobile transaction timeout (>" +
                           std::to_string(MOB_TIMEOUT_SEC) + "s)"}
        };
    }
    return future.get();
}