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

#include "ringpay.h"
#include "Database.h"
#include "falcon.h"

#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <mysqlx/xdevapi.h>

using namespace mysqlx;
using json = nlohmann::json;

inline constexpr double RING_SINGLE_LIMIT  = 2'000.0;
inline constexpr double RING_DAILY_LIMIT   = 5'000.0;
inline constexpr double RING_WARNING_LEVEL = 4'000.0;
inline constexpr double RING_MERCHANT_LIMIT = 3'000.0;
inline constexpr int    RING_RISK_BLOCK    = 70;

[[nodiscard]] static std::string generateToken() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::mt19937 gen{std::random_device{}()};
    return "RING-" + std::to_string(ms) + "-" +
           std::to_string(std::uniform_int_distribution<>(100000,999999)(gen));
}

[[nodiscard]] static std::string genTxnId() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "ring-txn-" + std::to_string(ms);
}

[[nodiscard]] static json err(std::string_view code, std::string_view msg = "") {
    json e{{"errorCode",std::string(code)}};
    if (!msg.empty()) e["message"] = std::string(msg);
    return e;
}

json processRingPayTransaction(const json& data) {
    json response;
    Database::ScopedConnection sc;
    Session& sess = *sc;
    Schema   db   = sess.getSchema("bankingdb");

    try {
        std::string pan      = data["pan"].get<std::string>();
        std::string expiry   = data.value("expiry","");
        double amount        = data["amount"].get<double>();
        double fee           = data.value("fee",0.0);
        std::string deviceId = data["deviceId"].get<std::string>();
        std::string ipAddr   = data.value("ipAddress","");
        std::string merchant = data["terminalId"].get<std::string>();

        Table cards       = db.getTable("cards");
        Table accounts    = db.getTable("accounts");
        Table tokenTable  = db.getTable("ringpay_tokens");
        Table ringTable   = db.getTable("transaction_ringpay");
        Table masterTable = db.getTable("transactions");

        // ── Card validation ───────────────────────────────────────────────
        auto cardRes = cards.select("account_number","status","card_priority")
            .where("pan=:p AND expiry=:e").bind("p",pan).bind("e",expiry).execute();
        if (cardRes.count() == 0)
            return err("ERR_INVALID_CARD","Card not found");

        Row cardRow  = cardRes.fetchOne();
        std::string accNo   = cardRow[0].get<std::string>();
        std::string cStatus = cardRow[1].get<std::string>();
        std::string cPrio   = cardRow[2].get<std::string>();

        if (cStatus != "ACTIVE" || cPrio != "PRIMARY")
            return err("ERR_CARD_RULE","Only PRIMARY ACTIVE card allowed for RingPay");

        // ── Account freeze check ─────────────────────────────────────────
        bool frozen = accounts.select("is_frozen")
            .where("account_number=:a").bind("a",accNo).execute().fetchOne()[0].get<bool>();
        if (frozen) return err("ERR_ACCOUNT_FROZEN","Account is frozen");

        // ── Falcon fraud check (NEW in v3) ────────────────────────────────
        Falcon falcon(sess);
        std::string fraudReason;
        std::string txnId = genTxnId();
        if (falcon.checkFraud(accNo, amount, fraudReason, FalconChannel::RINGPAY)) {
            std::string clientId = data.value("clientTxnId", txnId);
            falcon.logFraud(txnId, clientId, deviceId, "", accNo, amount, fraudReason);
            return {{"transactionId",txnId},{"status","DECLINED"},{"message",fraudReason}};
        }

        // ── Token management ──────────────────────────────────────────────
        auto tokRes = tokenTable.select("token")
            .where("account_number=:a AND status='ACTIVE' AND expires_at > NOW()")
            .bind("a",accNo).execute();
        std::string token;
        if (tokRes.count() == 0) {
            token = generateToken();
            tokenTable.insert("token","card_pan","account_number")
                .values(token, pan, accNo).execute();
        } else {
            token = tokRes.fetchOne()[0].get<std::string>();
        }

        // ── Limits ────────────────────────────────────────────────────────
        if (amount > RING_SINGLE_LIMIT)
            return err("ERR_SINGLE_LIMIT","Max " + std::to_string(RING_SINGLE_LIMIT) + " per RingPay txn");

        double usedToday = ringTable.select("IFNULL(SUM(amount),0)")
            .where("account_number=:a AND DATE(created_at)=CURDATE() AND status='SUCCESS'")
            .bind("a",accNo).execute().fetchOne()[0].get<double>();

        if (usedToday + amount > RING_DAILY_LIMIT)
            return err("ERR_DAILY_LIMIT","Daily RingPay limit exceeded");

        if (usedToday + amount > RING_WARNING_LEVEL)
            response["warning"] = "Approaching daily RingPay limit";

        double merchantUsed = ringTable.select("IFNULL(SUM(amount),0)")
            .where("account_number=:a AND merchant_id=:m AND DATE(created_at)=CURDATE()")
            .bind("a",accNo).bind("m",merchant).execute().fetchOne()[0].get<double>();
        if (merchantUsed + amount > RING_MERCHANT_LIMIT)
            return err("ERR_MERCHANT_LIMIT","Merchant daily limit exceeded");

        // ── Risk scoring ──────────────────────────────────────────────────
        int risk = 0;
        if (amount > 1500)     risk += 40;
        if (usedToday > 3000)  risk += 40;
        if (risk >= RING_RISK_BLOCK)
            return {{"status","BLOCKED"},{"reason","High risk score: " + std::to_string(risk)}};

        // ── Balance ───────────────────────────────────────────────────────
        double balance = accounts.select("balance")
            .where("account_number=:a").bind("a",accNo).execute().fetchOne()[0].get<double>();
        if (balance < amount + fee)
            return err("ERR_INSUFFICIENT_FUNDS","Insufficient balance");

        double newBal = balance - (amount+fee);
        accounts.update().set("balance",newBal)
            .where("account_number=:a").bind("a",accNo).execute();

        auto ringRes = ringTable.insert(
            "transaction_id","token","account_number",
            "amount","fee","status","message",
            "device_id","ip_address","merchant_id")
            .values(txnId,token,accNo,amount,fee,
                    "PROCESSING","RingPay Processing",
                    deviceId,ipAddr,merchant)
            .execute();
        long childId = ringRes.getAutoIncrementValue();

        // ── Simulate random failure (10% chance) ─────────────────────────
        std::mt19937 g{std::random_device{}()};
        bool failed = (std::uniform_int_distribution<>(0,9)(g) == 0);
        if (failed) {
            accounts.update().set("balance",balance)
                .where("account_number=:a").bind("a",accNo).execute();
            ringTable.update()
                .set("status","FAILED").set("reversal_status","REVERSED")
                .where("transaction_id=:t").bind("t",txnId).execute();
            return {{"status","FAILED"},{"message","Network failure — auto reversed"}};
        }

        ringTable.update().set("status","SUCCESS")
            .where("transaction_id=:t").bind("t",txnId).execute();
        masterTable.insert("table_name","reference_id","status")
            .values("transaction_ringpay", childId, "SUCCESS").execute();

        response["status"]        = "SUCCESS";
        response["transactionId"] = txnId;
        response["token"]         = token;
        response["balanceAfter"]  = newBal;
        response["riskScore"]     = risk;
        return response;

    } catch (const std::exception& e) {
        return err("ERR_EXCEPTION", e.what());
    }
}