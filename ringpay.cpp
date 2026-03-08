/*
* RingPay (Wearable Contactless Payment) Processing Flow:
 *
 * 1. Customer taps wearable device linked to PRIMARY active card.
 * 2. System validates card status, priority, and linked account state.
 * 3. Secure payment token fetched or generated for the device.
 * 4. Multiple safety checks enforced:
 *    • Per-transaction spending limit
 *    • Daily spending limit
 *    • Merchant-specific spending limit
 *    • Real-time risk scoring based on usage patterns
 * 5. Account balance verified before debit.
 * 6. Amount debited and transaction recorded in processing state.
 * 7. Random failure simulation may trigger automatic reversal.
 * 8. Successful transactions finalized and linked to master records.
 * 9. Response returned with token, risk score, and updated balance.
 *
 * Designed for secure low-latency NFC wearable payments.
 */
#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>

using namespace mysqlx;
using json = nlohmann::json;

// ---------------- TOKEN GENERATOR ----------------
static std::string generateToken() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> d(100000, 999999);

    std::ostringstream oss;
    oss << "RING-" << ms << "-" << d(gen);
    return oss.str();
}

// ---------------- TRANSACTION ID GENERATOR ----------------
static std::string generateRingTxnId() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count();

    return "ring-txn-" + std::to_string(ms);
}

// ---------------- MAIN RINGPAY PROCESS ----------------
json processRingPayTransaction(const json& data) {
    json response;

    try {
        // -------- INPUT --------
        std::string pan        = data["card"]["pan"];
        std::string expiry    = data["card"]["expiry"];
        double amount         = data["amount"];
        double fee            = data["fee"];
        std::string deviceId  = data["deviceId"];
        std::string ipAddr    = data["ipAddress"];
        std::string merchant  = data["merchantId"];

        // -------- DB SESSION --------
        Session sess("localhost", 33060, "root", "Rohan@5649");
        Schema db = sess.getSchema("bankingdb");

        Table cards        = db.getTable("cards");
        Table accounts     = db.getTable("accounts");
        Table tokenTable   = db.getTable("ringpay_tokens");
        Table ringTable    = db.getTable("transaction_ringpay");
        Table masterTable  = db.getTable("transactions");

        // -------- CARD VALIDATION --------
        RowResult cardRes = cards.select("account_number", "status", "card_priority")
            .where("pan=:p AND expiry=:e")
            .bind("p", pan)
            .bind("e", expiry)
            .execute();

        if (cardRes.count() == 0)
            return {{"errorCode","ERR_INVALID_CARD"},{"message","Card not found"}};

        Row cardRow = cardRes.fetchOne();
        std::string accNo = cardRow[0].get<std::string>();

        if (cardRow[1].get<std::string>() != "ACTIVE" ||
            cardRow[2].get<std::string>() != "PRIMARY")
            return {{"errorCode","ERR_CARD_RULE"},{"message","Only PRIMARY ACTIVE card allowed"}};

        // -------- ACCOUNT FREEZE CHECK --------
        bool isFrozen = accounts.select("is_frozen")
            .where("account_number=:a")
            .bind("a", accNo)
            .execute().fetchOne()[0].get<bool>();

        if (isFrozen)
            return {{"errorCode","ERR_ACCOUNT_FROZEN"},{"message","Account is frozen"}};

        // -------- TOKEN FETCH / CREATE --------
        RowResult tokRes = tokenTable.select("token")
            .where("account_number=:a AND status='ACTIVE' AND expires_at > NOW()")
            .bind("a", accNo)
            .execute();

        std::string token;

        if (tokRes.count() == 0) {
            token = generateToken();
            tokenTable.insert("token","card_pan","account_number")
                .values(token, pan, accNo)
                .execute();   // expires_at set by trigger
        } else {
            token = tokRes.fetchOne()[0].get<std::string>();
        }

        // -------- SINGLE TXN LIMIT --------
        if (amount > 2000)
            return {{"errorCode","ERR_SINGLE_LIMIT"},{"message","Max 2000 per transaction"}};

        // -------- DAILY LIMIT --------
        double usedToday = ringTable.select("IFNULL(SUM(amount),0)")
            .where("account_number=:a AND DATE(created_at)=CURDATE() AND status='SUCCESS'")
            .bind("a", accNo)
            .execute().fetchOne()[0].get<double>();

        if ((usedToday + amount) > 4000)
            response["warning"] = "You are nearing today's daily limit";

        if ((usedToday + amount) > 5000)
            return {{"errorCode","ERR_DAILY_LIMIT"},{"message","Daily limit exceeded"}};

        // -------- MERCHANT LIMIT --------
        double merchantUsed = ringTable.select("IFNULL(SUM(amount),0)")
            .where("account_number=:a AND merchant_id=:m AND DATE(created_at)=CURDATE()")
            .bind("a", accNo)
            .bind("m", merchant)
            .execute().fetchOne()[0].get<double>();

        if ((merchantUsed + amount) > 3000)
            return {{"errorCode","ERR_MERCHANT_LIMIT"}};

        // -------- RISK SCORING --------
        int risk = 0;
        if (amount > 1500) risk += 40;
        if (usedToday > 3000) risk += 40;

        if (risk > 70)
            return {{"status","BLOCKED"},{"reason","High Risk Transaction"}};

        // -------- BALANCE CHECK --------
        double balance = accounts.select("balance")
            .where("account_number=:a")
            .bind("a", accNo)
            .execute().fetchOne()[0].get<double>();

        if (balance < (amount + fee))
            return {{"errorCode","ERR_INSUFFICIENT_FUNDS"}};

        // -------- DEBIT --------
        double newBal = balance - (amount + fee);

        accounts.update()
            .set("balance", newBal)
            .where("account_number=:a")
            .bind("a", accNo)
            .execute();

        // -------- INSERT PROCESSING TXN --------
        std::string txnId = generateRingTxnId();

        auto ringRes = ringTable.insert(
            "transaction_id","token","account_number",
            "amount","fee","status","message",
            "device_id","ip_address","merchant_id"
        )
        .values(
            txnId, token, accNo,
            amount, fee, "PROCESSING", "RingPay Processing",
            deviceId, ipAddr, merchant
        )
        .execute();

        long childId = ringRes.getAutoIncrementValue();

        // -------- RANDOM FAILURE + REVERSAL --------
        bool failed = (std::rand() % 10 == 0);

        if (failed) {
            accounts.update()
                .set("balance", balance)
                .where("account_number=:a")
                .bind("a", accNo)
                .execute();

            ringTable.update()
                .set("status","FAILED")
                .set("reversal_status","REVERSED")
                .where("transaction_id=:t")
                .bind("t", txnId)
                .execute();

            return {{"status","FAILED"},{"message","Reversed automatically"}};
        }

        // -------- SUCCESS FINALIZATION --------
        ringTable.update()
            .set("status","SUCCESS")
            .where("transaction_id=:t")
            .bind("t", txnId)
            .execute();

        masterTable.insert("table_name","reference_id","status")
            .values("transaction_ringpay", childId, "SUCCESS")
            .execute();

        // -------- RESPONSE --------
        response["status"] = "SUCCESS";
        response["transactionId"] = txnId;
        response["token"] = token;
        response["balanceAfter"] = newBal;
        response["riskScore"] = risk;

        sess.close();
        return response;
    }
    catch (const std::exception &e) {
        return {{"errorCode","ERR_EXCEPTION"},{"message", e.what()}};
    }
}
