/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* RINGPAY (WEARABLE CONTACTLESS PAYMENT) ENGINE – NFC TRANSACTION FLOW
*
* 1. PURPOSE:
*    - Handles contactless wearable payments (Ring / NFC devices).
*    - Designed for ultra-fast, low-latency transactions.
*    - Uses tokenization instead of exposing actual card details.
*
* 2. INPUT FLOW:
*
*    - Request contains:
*        → Encrypted PAN + expiry
*        → deviceId (wearable identifier)
*        → ipAddress (risk tracking)
*        → merchantId
*        → amount + fee
*
*    - PAN mapped internally to account using cards table.
*
* 3. CARD VALIDATION:
*
*    - Validate PAN + expiry
*    - Card must be:
*        → ACTIVE
*        → PRIMARY (only primary card allowed for RingPay)
*
*    - If invalid → transaction rejected
*
* 4. ACCOUNT VALIDATION:
*
*    - Fetch linked account
*    - Check account freeze status
*    - If frozen → transaction blocked
*
* 5. TOKENIZATION (SECURITY CORE):
*
*    - System uses token instead of PAN
*
*    FLOW:
*        → Check existing active token (not expired)
*        → If not found:
*            → Generate new token (RING-<timestamp>-<random>)
*            → Store in ringpay_tokens table
*
*    - Token used for all transaction references
*
* 6. LIMIT CONTROLS:
*
*    PER TRANSACTION LIMIT:
*        → Max ₹2000
*
*    DAILY LIMIT:
*        → Warning after ₹4000
*        → Hard stop at ₹5000
*
*    MERCHANT LIMIT:
*        → Max ₹3000 per merchant per day
*
* 7. RISK ENGINE (REAL-TIME):
*
*    - Risk score calculated dynamically:
*
*        → High amount (>1500)       → +40
*        → High daily usage (>3000)  → +40
*
*    - If risk > 70:
*        → Transaction BLOCKED
*
*    - Can be extended for:
*        → device fingerprinting
*        → geo-location checks
*        → behavioral patterns
*
* 8. BALANCE VALIDATION:
*
*    - Ensure sufficient balance (amount + fee)
*
* 9. TRANSACTION EXECUTION:
*
*    Step 1: Debit account balance
*
*    Step 2: Insert transaction with:
*        → status = "PROCESSING"
*
*    Step 3: Random failure simulation:
*        → If failure:
*            → Reverse balance
*            → Mark transaction FAILED
*            → reversal_status = REVERSED
*
*    Step 4: If success:
*        → Update status = SUCCESS
*        → Insert into master transactions table
*
* 10. RESPONSE:
*
*    - SUCCESS:
*        → transactionId
*        → token
*        → updated balance
*        → riskScore
*
*    - FAILURE:
*        → errorCode / message
*        → or reversal response
*
* 11. SECURITY NOTES:
*
*    - Tokenization ensures PAN is never exposed
*    - Device + IP tracking improves fraud detection
*    - No PIN required → relies on limits + risk engine
*
*    ⚠ PRODUCTION ENHANCEMENTS:
*        → Token expiry enforcement
*        → Device binding
*        → Strong fraud engine (Falcon integration)
*
* 12. DESIGN NOTES:
*
*    - Combines:
*        → Tokenization
*        → Risk scoring
*        → Limit enforcement
*        → Reversal mechanism
*
*    - Mimics real-world:
*        → Apple Pay / Google Pay / NFC rings
*
* 13. FUTURE ENHANCEMENTS:
*
*    - Add multi-device support
*    - Add biometric validation
*    - Integrate with fraud engine (Falcon)
*    - Add offline transaction support
*    - Add real-time notification system
*
* Unauthorized modification without understanding tokenization,
* risk scoring, and reversal logic is strictly discouraged.
*
* For implementation details:
* Email: rohanavinashsakhare@gmail.com
* Mobile: +91 9112765649
*/

#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>
#include "Database.h"

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
        Session& sess = Database::getSession();
        Schema db = Database::getSchema();

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

        //sess.close();
        return response;
    }
    catch (const std::exception &e) {
        return {{"errorCode","ERR_EXCEPTION"},{"message", e.what()}};
    }
}
