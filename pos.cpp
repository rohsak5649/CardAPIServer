// pos.cpp — STRICT SINGLE-REFUND IMPLEMENTATION
// - Refund allowed only once per original purchase (origClientTxnId).
// - Refund rows store original_purchase_id to make checks accurate.
// - Uses MySQL Connector/C++ X DevAPI (mysqlx).
// - ARM/mac-safe (int64_t), no broken sess.sql binds.

#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <mysqlx/xdevapi.h>
#include "json.hpp"

using namespace mysqlx;
using json = nlohmann::json;

static std::string genPosTxnId() {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int r = std::rand() % 9000 + 1000;
    std::ostringstream oss;
    oss << "pos-txn-" << ms << "-" << r;
    return oss.str();
}

json processPOSTransaction(const json &data) {
    json res;
    try {
        // Basic extraction
        std::string clientTxnId = data.value("clientTxnId", genPosTxnId());
        std::string txnType     = data.value("transactionType", std::string("PURCHASE"));
        std::string merchantId  = data.value("merchantId", std::string());
        std::string terminalId  = data.value("terminalId", std::string());
        std::string location    = data.value("location", std::string());
        double amount           = data.value("amount", 0.0);
        double fee              = data.value("fee", 0.0);
        std::string currency    = data.value("currency", std::string("USD"));

        if (!data.contains("card")) {
            res["errorCode"] = "ERR_MISSING_CARD";
            res["message"] = "card object required";
            return res;
        }

        json card = data["card"];
        std::string pan    = card.value("pan", std::string());
        std::string expiry = card.value("expiry", std::string());
        std::string cvv    = card.value("cvv", std::string());

        if (pan.empty() || expiry.empty()) {
            res["errorCode"] = "ERR_CARD_INCOMPLETE";
            res["message"] = "PAN and expiry required";
            return res;
        }

        // DB connection (adjust if your host/port/credentials differ)
        Session sess("localhost", 33060, "root", "Rohan@5649");
        Schema db = sess.getSchema("bankingdb");

        Table cards    = db.getTable("cards");
        Table accounts = db.getTable("accounts");
        Table posTbl   = db.getTable("transaction_pos");
        Table master   = db.getTable("transactions");

        // Validate card
        RowResult cardRes = cards.select("account_number","scheme","cvv","status")
            .where("pan = :p AND expiry = :e")
            .bind("p", pan)
            .bind("e", expiry)
            .execute();

        if (cardRes.count() == 0) {
            res["errorCode"] = "ERR_CARD_NOT_FOUND";
            res["message"] = "Card not found for given PAN/expiry.";
            return res;
        }

        Row cardRow = cardRes.fetchOne();
        std::string accNo      = cardRow[0].get<std::string>();
        std::string cardScheme = cardRow[1].isNull() ? std::string() : cardRow[1].get<std::string>();
        std::string dbCvv      = cardRow[2].isNull() ? std::string() : cardRow[2].get<std::string>();
        std::string cardStatus = cardRow[3].isNull() ? std::string() : cardRow[3].get<std::string>();

        if (cardStatus != "ACTIVE") {
            res["errorCode"] = "ERR_CARD_NOT_ACTIVE";
            res["message"] = "Card is not active.";
            return res;
        }

        // Optional CVV check (if provided)
        if (!cvv.empty()) {
            if (!dbCvv.empty() && cvv != dbCvv) {
                res["errorCode"] = "ERR_CVV_MISMATCH";
                res["message"] = "Provided CVV does not match stored CVV.";
                return res;
            }
        }

        // Fetch account details
        RowResult accRes = accounts.select("currency","balance")
            .where("account_number = :a")
            .bind("a", accNo)
            .execute();

        if (accRes.count() == 0) {
            res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            res["message"] = "Linked account not found.";
            return res;
        }

        Row accRow = accRes.fetchOne();
        std::string accCurrency = accRow[0].get<std::string>();
        double balance = accRow[1].get<double>();

        std::string txnId = genPosTxnId();
        std::string scope = (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

        // ----------------- PURCHASE -----------------
        if (txnType == "PURCHASE") {
            if (balance < (amount + fee)) {
                res["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
                res["message"] = "Not enough balance.";
                return res;
            }

            double newBal = balance - (amount + fee);

            // Update balance
            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            // Insert purchase row; original_purchase_id is NULL for purchases
            auto ins = posTbl.insert(
                "transaction_id","client_txn_id","merchant_id","terminal_id",
                "location","account_number","amount","fee","card_pan","card_scheme",
                "status","message","original_purchase_id"
            ).values(
                txnId, clientTxnId, merchantId, terminalId,
                location, accNo, amount, fee, pan, cardScheme,
                "SUCCESS", "POS purchase successful", nullptr
            ).execute();

            int64_t childId = ins.getAutoIncrementValue();

            // Insert master row
            master.insert("table_name","reference_id","status")
                .values("transaction_pos", childId, "SUCCESS")
                .execute();

            res["transactionId"] = txnId;
            res["status"] = "SUCCESS";
            res["balanceAfter"] = newBal;
            res["transactionScope"] = scope;
            return res;
        }

        // ----------------- REFUND (STRICT SINGLE REFUND) -----------------
        if (txnType == "REFUND") {
            if (!data.contains("origClientTxnId")) {
                res["errorCode"] = "ERR_MISSING_ORIGINAL";
                res["message"] = "origClientTxnId required for refund.";
                return res;
            }

            std::string origClient = data["origClientTxnId"].get<std::string>();

            // Locate original purchase row (must be a successful POS purchase)
            RowResult purchRes = posTbl.select("id","amount","card_pan")
                .where("client_txn_id = :cid AND message = 'POS purchase successful'")
                .bind("cid", origClient)
                .execute();

            if (purchRes.count() == 0) {
                res["errorCode"] = "ERR_PURCHASE_NOT_FOUND";
                res["message"] = "Original POS purchase not found for given origClientTxnId.";
                return res;
            }

            Row purchRow = purchRes.fetchOne();
            int64_t purchaseDbId = purchRow[0].get<int64_t>();
            double purchaseAmount = purchRow[1].get<double>();
            std::string purchasePan = purchRow[2].get<std::string>();

            // PAN must match original purchase
            if (purchasePan != pan) {
                res["errorCode"] = "ERR_PAN_MISMATCH";
                res["message"] = "Refund PAN does not match original purchase PAN.";
                return res;
            }

            // Check if ANY refund already exists for this purchase (strict single refund)
            RowResult alreadyRefundedRes = posTbl.select("id")
                .where("original_purchase_id = :pid AND message = 'Refund successful'")
                .bind("pid", purchaseDbId)
                .execute();

            if (alreadyRefundedRes.count() > 0) {
                res["errorCode"] = "ERR_ALREADY_REFUNDED";
                res["message"] = "This purchase has already been refunded (strict single-refund policy).";
                return res;
            }

            // Optionally enforce refund amount <= purchase amount (we'll enforce)
            if (amount > purchaseAmount) {
                res["errorCode"] = "ERR_REFUND_EXCEEDS_PURCHASE";
                res["message"] = "Refund amount exceeds original purchase amount.";
                res["purchaseAmount"] = purchaseAmount;
                return res;
            }

            // Credit the account
            double newBal = balance + amount;
            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            // Insert refund row and set original_purchase_id = purchaseDbId
            auto insRefund = posTbl.insert(
                "transaction_id","client_txn_id","merchant_id","terminal_id",
                "location","account_number","amount","fee","card_pan","card_scheme",
                "status","message","original_purchase_id"
            ).values(
                txnId, clientTxnId, merchantId, terminalId, location,
                accNo, amount, 0.0, pan, cardScheme,
                "SUCCESS", "Refund successful", purchaseDbId
            ).execute();

            int64_t refundChildId = insRefund.getAutoIncrementValue();

            // Insert master row
            master.insert("table_name","reference_id","status")
                .values("transaction_pos", refundChildId, "SUCCESS")
                .execute();

            res["transactionId"] = txnId;
            res["status"] = "SUCCESS";
            res["balanceAfter"] = newBal;
            res["purchaseAmount"] = purchaseAmount;
            res["refundedAmount"] = amount;
            return res;
        }

        // Unsupported type
        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"] = "Unsupported POS transactionType. Only PURCHASE and REFUND allowed.";
        return res;
    }
    catch (const std::exception &e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"] = e.what();
        return res;
    }
}
