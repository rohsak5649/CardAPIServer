
/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* QR CODE PAYMENT PROCESSING ENGINE – PURCHASE & REFUND FLOW
*
* 1. PURPOSE:
*    - Handles QR-based payment transactions.
*    - Supports:
*        → PURCHASE (scan & pay)
*        → REFUND (linked to original QR transaction)
*
*    - Designed for UPI-like / scan-based payment systems.
*
* 2. INPUT SOURCES:
*
*    - QR data may come in two forms:
*        → Structured API request (JSON fields)
*        → Raw QR string (qrData)
*
*    - System extracts values from qrData:
*        → merchantId (mid)
*        → merchantName
*        → terminalId (tid)
*        → currency
*        → amount (optional)
*
*    - If fields exist in both:
*        → qrData takes precedence
*
* 3. PURCHASE FLOW:
*
*    Step 1: Validation
*        - Amount must be > 0
*        - accountNumber required (user-selected debit account)
*
*    Step 2: Account Fetch
*        - Validate account existence
*        - Fetch balance and currency
*
*    Step 3: Balance Check
*        - Ensure sufficient balance (amount + fee)
*
*    Step 4: Transaction Execution
*        - Debit account balance
*        - Insert QR purchase record:
*            → message = "QR purchase successful"
*        - Insert entry in master transactions table
*
*    Step 5: Response
*        - txnId
*        - updated balance
*        - merchant details
*        - transaction scope (DOMESTIC / INTERNATIONAL)
*
* 4. REFUND FLOW (STRICT SINGLE REFUND):
*
*    Step 1: Identify Original Transaction
*        - Uses origClientTxnId (client reference)
*
*    Step 2: Validate Original Purchase
*        - Must be a successful QR purchase
*        - Fetch:
*            → purchase amount
*            → account number
*
*    Step 3: Account Validation
*        - Refund must be processed to same account
*
*    Step 4: Refund Policy (STRICT)
*
*        - Only ONE refund allowed per purchase
*
*        Validation:
*            → Check if any refund exists:
*                WHERE orig_ref_id = purchaseId
*
*            → If exists → reject (already refunded)
*
*    Step 5: Amount Validation
*        - Refund amount ≤ purchase amount
*
*    Step 6: Refund Execution
*        - Credit account balance
*        - Insert refund record:
*            → message = "QR refund successful"
*            → orig_ref_id = purchaseId
*
*        - Insert into master transactions table
*
*    Step 7: Response
*        - txnId
*        - updated balance
*        - purchaseAmount
*        - refundedAmount
*
* 5. TRANSACTION LINKING:
*
*    - Purchase:
*        → orig_ref_id = NULL
*
*    - Refund:
*        → orig_ref_id = purchaseId
*
*    - Enables traceability and audit tracking
*
* 6. ERROR HANDLING:
*
*    - ERR_INVALID_AMOUNT
*    - ERR_MISSING_ACCOUNT
*    - ERR_ACCOUNT_NOT_FOUND
*    - ERR_INSUFFICIENT_FUNDS
*    - ERR_MISSING_ORIG_REF
*    - ERR_PURCHASE_NOT_FOUND
*    - ERR_ACCOUNT_MISMATCH
*    - ERR_ALREADY_REFUNDED
*    - ERR_REFUND_EXCEEDS
*    - ERR_INVALID_TYPE
*
* 7. DESIGN NOTES:
*
*    - Lightweight QR parser (key=value format)
*    - Flexible input handling (API + raw QR)
*    - Uses Database singleton for DB operations
*
* 8. SECURITY NOTES:
*
*    - No PAN usage → safer than card-based systems
*    - Account-based debit reduces card exposure risk
*    - Input validation prevents malformed QR abuse
*
* 9. LIMITATIONS:
*
*    - Supports only single refund (no partial refunds)
*    - QR parsing is basic (not full EMVCo compliant)
*
* 10. FUTURE ENHANCEMENTS:
*
*    - Support partial refunds (like POS/ECOM)
*    - EMVCo QR standard parsing
*    - Fraud detection integration (Falcon)
*    - Dynamic QR support (merchant-generated)
*    - Add payer authentication (PIN/OTP)
*
* Unauthorized modification without understanding QR parsing,
* refund policy, and transaction linkage is discouraged.
*
* For implementation details:
* Email: rohanavinashsakhare@gmail.com
* Mobile: +91 9112765649
*/

#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <random>
#include <algorithm>
#include <mysqlx/xdevapi.h>
#include "json.hpp"
#include "Database.h"

using namespace mysqlx;
using json = nlohmann::json;

static std::string genQRTxnId() {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    int r = std::rand() % 9000 + 1000;
    std::ostringstream oss;
    oss << "qr-txn-" << ms << "-" << r;
    return oss.str();
}

// Basic helper: lowercase a string
static std::string lower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

// Very small parser for "qrData" where keys may appear like "mid=XXX;merchantName=YYY;amount=12.34;currency=AUD"
// It searches for key patterns and returns value if found (not a full EMV parser).
static std::string find_kv(const std::string &qrData, const std::string &key) {
    std::string data = qrData;
    // normalize separators to semicolon for easier split
    // allow both '&' and ';' and '|' as separators
    for (char &c : data) if (c == '&' || c == '|' ) c = ';';

    std::string lowerData = lower(data);
    std::string lowerKey = lower(key) + "=";

    size_t pos = lowerData.find(lowerKey);
    if (pos == std::string::npos) return std::string();

    pos += lowerKey.size();
    size_t end = lowerData.find(';', pos);
    if (end == std::string::npos) end = data.size();
    std::string val = data.substr(pos, end - pos);
    // trim whitespace
    size_t a = val.find_first_not_of(" \t\n\r");
    size_t b = val.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return std::string();
    return val.substr(a, b - a + 1);
}

json processQRCodePayment(const json &data) {
    json res;
    try {
        // Basic request fields
        std::string clientTxnId = data.value("clientTxnId", genQRTxnId());
        std::string txnType     = data.value("transactionType", std::string("PURCHASE"));
        std::string qrData      = data.value("qrData", std::string());
        std::string merchantName = data.value("merchantName", std::string());
        std::string mid         = data.value("mid", std::string());
        std::string tid         = data.value("tid", std::string());
        double amount           = data.value("amount", 0.0); // may be 0 if qr has amount or user enters
        double fee              = data.value("fee", 0.0);
        std::string currency    = data.value("currency", std::string("AUD"));
        std::string accountNumber = data.value("accountNumber", std::string()); // debit account selected by user

        // Try to extract from qrData if provided
        if (!qrData.empty()) {
            // prefer fields found in qrData, otherwise keep provided fields
            std::string kv;
            kv = find_kv(qrData, "mid");
            if (!kv.empty()) mid = kv;
            kv = find_kv(qrData, "merchantName");
            if (!kv.empty()) merchantName = kv;
            kv = find_kv(qrData, "tid");
            if (!kv.empty()) tid = kv;
            kv = find_kv(qrData, "currency");
            if (!kv.empty()) currency = kv;
            kv = find_kv(qrData, "amount");
            if (!kv.empty()) {
                try { amount = std::stod(kv); } catch (...) { /* ignore parse errors */ }
            }
        }

        // validation
        if (txnType == "PURCHASE" && amount <= 0.0) {
            res["errorCode"] = "ERR_INVALID_AMOUNT";
            res["message"] = "Purchase amount must be > 0 (either in qrData or in request).";
            return res;
        }
        if (accountNumber.empty()) {
            res["errorCode"] = "ERR_MISSING_ACCOUNT";
            res["message"] = "accountNumber is required to perform QR debit.";
            return res;
        }

        // DB connection (adjust credentials if needed)
        Session& sess = Database::getSession();
        Schema db = Database::getSchema();

        Table accounts = db.getTable("accounts");
        Table qrTable  = db.getTable("transaction_qrcode");
        Table master   = db.getTable("transactions");

        // Fetch account
        RowResult accRes = accounts.select("currency","balance")
            .where("account_number = :a")
            .bind("a", accountNumber)
            .execute();

        if (accRes.count() == 0) {
            res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            res["message"] = "Debit account not found.";
            return res;
        }

        Row accRow = accRes.fetchOne();
        std::string accCurrency = accRow[0].get<std::string>();
        double balance = accRow[1].get<double>();

        std::string txnId = genQRTxnId();
        std::string scope = (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

        // ---------------- PURCHASE ----------------
        if (txnType == "PURCHASE") {
            if (balance < (amount + fee)) {
                res["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
                res["message"] = "Insufficient balance.";
                return res;
            }

            double newBal = balance - (amount + fee);

            // Update account balance
            accounts.update().set("balance", newBal)
                .where("account_number = :a").bind("a", accountNumber)
                .execute();

            // Insert QR purchase row
            auto ins = qrTable.insert(
                "transaction_id","client_txn_id","qr_raw_data","merchant_name",
                "merchant_id","terminal_id","account_number","amount","fee",
                "currency","transaction_scope","status","message"
            ).values(
                txnId, clientTxnId, qrData, merchantName,
                mid, tid, accountNumber, amount, fee,
                currency, scope, "SUCCESS", "QR purchase successful"
            ).execute();

            int64_t childId = ins.getAutoIncrementValue();

            master.insert("table_name","reference_id","status")
                .values("transaction_qrcode", childId, "SUCCESS").execute();

            res["transactionId"] = txnId;
            res["status"] = "SUCCESS";
            res["balanceAfter"] = newBal;
            res["merchantName"] = merchantName;
            res["merchantId"] = mid;
            res["transactionScope"] = scope;
            return res;
        }

        // ---------------- REFUND (strict single refund) ----------------
        if (txnType == "REFUND") {
            if (!data.contains("origClientTxnId")) {
                res["errorCode"] = "ERR_MISSING_ORIG_REF";
                res["message"] = "origClientTxnId required for refund.";
                return res;
            }
            std::string origClient = data["origClientTxnId"].get<std::string>();

            // find original purchase row
            RowResult pRes = qrTable.select("id","amount","account_number")
                .where("client_txn_id = :cid AND message = 'QR purchase successful'")
                .bind("cid", origClient)
                .execute();

            if (pRes.count() == 0) {
                res["errorCode"] = "ERR_PURCHASE_NOT_FOUND";
                res["message"] = "Original QR purchase not found.";
                return res;
            }

            Row pRow = pRes.fetchOne();
            int64_t purchaseId = pRow[0].get<int64_t>();
            double purchaseAmount = pRow[1].get<double>();
            std::string purchAcc = pRow[2].get<std::string>();

            // ensure refund is attempted against same account
            if (purchAcc != accountNumber) {
                res["errorCode"] = "ERR_ACCOUNT_MISMATCH";
                res["message"] = "Refund account does not match original purchase account.";
                return res;
            }

            // check if any refund already exists (strict single-refund policy)
            RowResult already = qrTable.select("id")
                .where("orig_ref_id = :pid AND message = 'QR refund successful'")
                .bind("pid", purchaseId)
                .execute();

            if (already.count() > 0) {
                res["errorCode"] = "ERR_ALREADY_REFUNDED";
                res["message"] = "This purchase has already been refunded (strict single-refund).";
                return res;
            }

            // enforce refund amount <= purchase amount
            if (amount > purchaseAmount) {
                res["errorCode"] = "ERR_REFUND_EXCEEDS";
                res["message"] = "Refund amount exceeds original purchase amount.";
                res["purchaseAmount"] = purchaseAmount;
                return res;
            }

            // credit account
            double newBal = balance + amount;
            accounts.update().set("balance", newBal)
                .where("account_number = :a").bind("a", accountNumber)
                .execute();

            auto ins = qrTable.insert(
                "transaction_id","client_txn_id","qr_raw_data","merchant_name",
                "merchant_id","terminal_id","account_number","amount","fee",
                "currency","transaction_scope","status","message","orig_ref_id"
            ).values(
                txnId, clientTxnId, qrData, merchantName,
                mid, tid, accountNumber, amount, 0.0,
                currency, scope, "SUCCESS", "QR refund successful", purchaseId
            ).execute();

            int64_t childId = ins.getAutoIncrementValue();

            master.insert("table_name","reference_id","status")
                .values("transaction_qrcode", childId, "SUCCESS").execute();

            res["transactionId"] = txnId;
            res["status"] = "SUCCESS";
            res["balanceAfter"] = newBal;
            res["purchaseAmount"] = purchaseAmount;
            res["refundedAmount"] = amount;
            return res;
        }

        // Unsupported type
        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"] = "Unsupported QR transactionType";
        return res;
    }
    catch (const std::exception &e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"] = e.what();
        return res;
    }
}
