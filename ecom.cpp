/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* ECOM TRANSACTION PROCESSING FLOW:
*
* 1. Transaction request received from ECOM channel API.
* 2. Input validation performed (transaction type, required fields, structure).
*
* 3. PAN & PIN FLOW (SECURE PROCESSING):
*    - Encrypted PAN received from client.
*    - PAN is decrypted internally using AES-GCM (panencrypted module).
*    - If decryption fails → transaction declined.
*    - PIN verification performed using PIN service.
*    - Optional validation of expiry & CVV (if provided).
*    - Card details mapped to account using cards table.
*
* 4. FRAUD CHECK (FALCON ENGINE):
*    - Same-second transaction detection.
*    - Transaction velocity checks (multiple transactions within short duration).
*    - Fraud events logged in transaction_falcon table.
*    - If fraud detected → transaction declined.
*
* 5. ACCOUNT VALIDATIONS:
*    - Account existence validation.
*    - Account status check (active/frozen).
*    - Balance validation for purchase transactions.
*
* 6. CURRENCY & SCOPE HANDLING:
*    - Account currency resolved using currency table.
*    - Transaction scope determined:
*        → DOMESTIC / INTERNATIONAL
*
* 7. PURCHASE FLOW:
*    - Debit account balance (amount + fee).
*    - Insert record in transaction_ecom table.
*    - Insert entry in master transactions table.
*    - Update last_transaction_time in cards table.
*
* 8. REFUND FLOW (TXN ID BASED - ENHANCED):
*    - Refund initiated using originalTxnId (purchase transaction).
*    - Original transaction fetched and validated.
*    - PAN validation against original transaction.
*
*    REFUND VALIDATIONS:
*    - Prevent refund if already fully refunded (flag = RF).
*    - Validate refund does not exceed remaining refundable amount.
*
*    REFUND PROCESSING:
*    - Credit amount back to account.
*    - Update original transaction:
*        → refunded_amount updated
*        → flag updated (N → PR → RF)
*    - Insert new refund transaction with reference_txn_id.
*
*    FLAG DEFINITIONS:
*        N  → No refund
*        PR → Partial refund
*        RF → Fully refunded
*
* 9. TRANSACTION RECORDING:
*    - All transactions stored in transaction_ecom table.
*    - Refund transactions linked via reference_txn_id.
*    - Master entry recorded in transactions table.
*
* 10. RESPONSE:
*    - SUCCESS → txnId + updated balance / refund status
*    - FAILURE → errorCode + message
*
* SECURITY NOTES:
* - PAN is never exposed or stored in plain text externally.
* - All sensitive operations handled securely in backend.
* - Encryption standard: AES-256-GCM
* - PIN validation ensures cardholder authentication.
*
* DESIGN NOTES:
* - Refunds are strictly txnId-based (no dependency on orderId).
* - Supports partial and full refunds.
* - Ensures audit traceability via reference_txn_id.
*
* Unauthorized modification without understanding transaction,
* refund logic, and fraud handling is strongly discouraged.
*
* For implementation details, contact: +91 9112765649
*/

#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <future>

#include <mysqlx/xdevapi.h>
#include "json.hpp"
#include "Database.h"

#include "falcon.h"
#include "pin.h"
#include "panencrypted.h"

using namespace mysqlx;
using json = nlohmann::json;

// ================= HELPERS =================
std::string normalizeExpiry(const std::string& exp) {
    if (exp.empty()) return "";

    if (exp.find("/") != std::string::npos) {
        return "20" + exp.substr(3,2) + "-" + exp.substr(0,2);
    }
    if (exp.length() == 4) {
        return "20" + exp.substr(2,2) + "-" + exp.substr(0,2);
    }
    return exp;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// ================= TXN ID =================
static std::string genTxnId() {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int r = rand() % 9000 + 1000;
    std::ostringstream oss;
    oss << "ecom-txn-" << ms << "-" << r;
    return oss.str();
}

// ================= CORE =================
json processECOMTransactionCore(const json &data) {

    json res;

    try {
        std::string clientTxnId = data.value("clientTxnId", genTxnId());
        std::string txnType     = data.value("transactionType", "PURCHASE");
        std::string merchantId  = data.value("merchantId", "");
        std::string orderId     = data.value("orderId", "");
        double amount           = data.value("amount", 0.0);
        double fee              = data.value("fee", 0.0);
        std::string currency    = data.value("currency", "USD");

        Session& sess = Database::getSession();
        Schema db = Database::getSchema();

        Table cards = db.getTable("cards");
        Table accounts = db.getTable("accounts");
        Table ecom = db.getTable("transaction_ecom");
        Table master = db.getTable("transactions");
        Table currencyTbl = db.getTable("currency");

        sess.startTransaction();

        // ================= CARD INPUT =================
        std::string pan = "";
        std::string expiry = "";
        std::string cvv = "";

        if (!data.contains("card")) {
            res["errorCode"] = "ERR_MISSING_CARD";
            res["message"] = "card object required";
            sess.rollback();   // ✅ add this
            return res;
        }

        json card = data["card"];

        if (card.contains("pan") && card.contains("pin")) {

            std::string encryptedPan = card["pan"];
            std::string inputPin = card["pin"];

            // decrypt PAN
            try {
                auto& panService = PANEncryptionService::getInstance();
                pan = panService.decryptPAN(encryptedPan);
            } catch (const std::exception &e) {
                res["errorCode"] = "ERR_INVALID_ENCRYPTED_PAN";
                res["message"] = e.what();
                sess.rollback();
                return res;
            }

            // PIN verify
            auto& pinService = PINService::getInstance();
            if (!pinService.verifyPIN(pan, inputPin)) {
                res["errorCode"] = "ERR_INVALID_PIN";
                res["message"] = "PIN verification failed";
                sess.rollback();
                return res;
            }

            expiry = card.value("expiry", "");
            cvv    = card.value("cvv", "");

        } else {
            pan = card.value("pan", "");
            expiry = card.value("expiry", "");
            cvv = card.value("cvv", "");
        }

        if (pan.empty()) {
            res["errorCode"] = "ERR_CARD_INCOMPLETE";
            res["message"] = "PAN required";
            sess.rollback();   // ✅ add
            return res;
        }
        if (pan.length() != 16) {
            res["errorCode"] = "ERR_INVALID_PAN";
            res["message"] = "Invalid PAN format";
            sess.rollback();
            return res;
        }

        // ================= CARD FETCH =================
        RowResult cardRes = cards
            .select("account_number","scheme","expiry","cvv","status")
            .where("pan = :p")
            .bind("p", pan)
            .execute();

        if (cardRes.count() == 0) {
            res["errorCode"] = "ERR_CARD_NOT_FOUND";
            res["message"] = "Invalid PAN";
            sess.rollback();
            return res;
        }

        Row cardRow = cardRes.fetchOne();

        std::string accNo      = cardRow[0].get<std::string>();
        std::string cardScheme = cardRow[1].isNull() ? "" : cardRow[1].get<std::string>();
        std::string dbExpiry   = cardRow[2].isNull() ? "" : cardRow[2].get<std::string>();
        std::string dbCvv      = cardRow[3].isNull() ? "" : cardRow[3].get<std::string>();
        std::string status     = cardRow[4].get<std::string>();

        if (status != "ACTIVE") {
            res["errorCode"] = "ERR_CARD_BLOCKED";
            res["message"] = "Card inactive";
            sess.rollback();
            return res;
        }

        // ================= EXPIRY VALIDATION =================
        if (!expiry.empty()) {
            if (normalizeExpiry(expiry) != normalizeExpiry(dbExpiry)) {
                res["errorCode"] = "ERR_EXPIRY_MISMATCH";
                res["message"] = "Invalid expiry";
                sess.rollback();
                return res;
            }
        }

        // ================= CVV VALIDATION =================
        if (!cvv.empty()) {
            if (trim(cvv) != trim(dbCvv)) {
                res["errorCode"] = "ERR_CVV_MISMATCH";
                res["message"] = "Invalid CVV";
                sess.rollback();
                return res;
            }
        }

        // ================= ACCOUNT =================
        RowResult accRes = accounts
            .select("currency_id","balance")
            .where("account_number = :a")
            .bind("a", accNo)
            .execute();

        if (accRes.count() == 0) {
            res["errorCode"] = "ERR_ACC_NOT_FOUND";
            res["message"] = "Account missing";
            sess.rollback();
            return res;
        }

        Row accRow = accRes.fetchOne();
        int currencyId = accRow[0].get<int>();
        double balance = accRow[1].get<double>();

        RowResult curRes = currencyTbl
            .select("currency_code")
            .where("currency_id = :id")
            .bind("id", currencyId)
            .execute();

        std::string accCurrency = curRes.fetchOne()[0].get<std::string>();

        // ================= FALCON =================
        Falcon falcon(sess);
        std::string fraudReason;

        if (falcon.checkFraud(accNo, amount, fraudReason)) {

            std::string txnId = genTxnId();

            falcon.logFraud(txnId, clientTxnId, "", "", accNo, amount, fraudReason);

            sess.commit();

            res["txnId"] = txnId;
            res["status"] = "DECLINED";
            res["message"] = fraudReason;
            return res;
        }

        std::string scope = (currency == accCurrency) ? "DOMESTIC" : "INTERNATIONAL";
        std::string txnId = genTxnId();

        // ================= PURCHASE =================
        if (txnType == "PURCHASE") {

            if (balance < amount + fee) {
                res["errorCode"] = "ERR_NO_FUNDS";
                res["message"] = "Insufficient balance";
                sess.rollback();
                return res;
            }

            double newBal = balance - (amount + fee);

            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            auto ins = ecom.insert(
                "transaction_id","client_txn_id","merchant_id","order_id",
                "currency","transaction_scope","account_number",
                "amount","fee","card_pan","card_scheme","status","message"
            )
            .values(
                txnId, clientTxnId, merchantId, orderId,
                currency, scope, accNo,
                amount, fee, pan, cardScheme,
                "SUCCESS", "E-commerce purchase successful"
            )
            .execute();

            master.insert("table_name","reference_id","status")
                .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
                .execute();

            sess.commit();

            // last txn time
            try {
                cards.update()
                    .set("last_transaction_time", mysqlx::expr("NOW()"))
                    .where("pan = :p")
                    .bind("p", pan)
                    .execute();
            } catch (...) {}

            res["txnId"] = txnId;
            res["status"] = "SUCCESS";
            res["balanceAfter"] = newBal;
            return res;
        }

        // ================= REFUND =================
// ================= REFUND =================
if (txnType == "REFUND") {

    std::string originalTxnId = data.value("originalTxnId", "");

    if (originalTxnId.empty()) {
        res["errorCode"] = "ERR_MISSING_ORIGINAL_TXN";
        res["message"]   = "originalTxnId is required";
        sess.rollback();
        return res;
    }

    // 🔍 Fetch original purchase
    RowResult pRes = ecom
        .select("transaction_id","amount","card_pan","refunded_amount","flag","account_number")
        .where("transaction_id = :t AND message = 'E-commerce purchase successful'")
        .bind("t", originalTxnId)
        .execute();

    if (pRes.count() == 0) {
        res["errorCode"] = "ERR_PURCHASE_NOT_FOUND";
        res["message"]   = "Invalid originalTxnId";
        sess.rollback();
        return res;
    }

    Row p = pRes.fetchOne();

    std::string purchaseTxnId = p[0].get<std::string>();
    double purchaseAmt        = p[1].get<double>();
    std::string purchasePan   = p[2].get<std::string>();
    double refundedAmount     = p[3].isNull() ? 0.0 : p[3].get<double>();
    std::string flag          = p[4].isNull() ? "N" : p[4].get<std::string>();

    // 🔐 PAN validation
    if (purchasePan != pan) {
        res["errorCode"] = "ERR_PAN_MISMATCH";
        res["message"]   = "Refund PAN does not match purchase";
        sess.rollback();
        return res;
    }

    // 🚫 Already fully refunded
    if (flag == "RF") {
        res["errorCode"] = "ERR_FULLY_REFUNDED";
        res["message"]   = "Already fully refunded";
        sess.rollback();
        return res;
    }

    // 🚫 Exceeds limit
    if (refundedAmount + amount > purchaseAmt) {
        res["errorCode"] = "ERR_REFUND_EXCEEDS";
        res["message"]   = "Refund amount exceeds remaining refundable balance";
        sess.rollback();
        return res;
    }

    // 💰 Apply refund
    double newBal = balance + amount;

    accounts.update()
        .set("balance", newBal)
        .where("account_number = :a")
        .bind("a", accNo)
        .execute();

    // 🔥 Update original txn
    std::string newFlag = ((refundedAmount + amount) == purchaseAmt) ? "RF" : "PR";

    sess.sql(
        "UPDATE bankingdb.transaction_ecom "
        "SET refunded_amount = refunded_amount + ?, flag = ? "
        "WHERE transaction_id = ?"
    )
    .bind(amount)
    .bind(newFlag)
    .bind(originalTxnId)
    .execute();

    // 🧾 Insert refund record
    auto ins = ecom.insert(
        "transaction_id","client_txn_id","merchant_id","order_id",
        "currency","transaction_scope","account_number",
        "amount","fee","card_pan","card_scheme","status","message","reference_txn_id"
    )
    .values(
        txnId, clientTxnId, merchantId, orderId,
        currency, scope, accNo, amount, 0.0,
        pan, cardScheme, "SUCCESS", "Refund successful", originalTxnId
    )
    .execute();

    master.insert("table_name","reference_id","status")
        .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
        .execute();

    sess.commit();

    res["txnId"] = txnId;
    res["status"] = "SUCCESS";
    res["refundedTotal"] = refundedAmount + amount;
    res["flag"] = newFlag;
    return res;
}

        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"] = "Invalid type";
        return res;



    } catch (const std::exception &e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"] = e.what();
        return res;
    }
}

// ================= WRAPPER =================
json processECOMTransaction(const json &data) {

    auto future = std::async(std::launch::async, processECOMTransactionCore, data);

    if (future.wait_for(std::chrono::seconds(6)) == std::future_status::timeout) {
        json res;
        res["status"] = "DECLINED";
        res["errorCode"] = "ERR_TIMEOUT";
        res["message"] = "Transaction timeout";
        return res;
    }

    return future.get();
}