#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <mysqlx/xdevapi.h>
#include "json.hpp"

using namespace mysqlx;
using json = nlohmann::json;

// Generate unique TXN ID
static std::string genTxnId() {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int r = rand() % 9000 + 1000;
    std::ostringstream oss;
    oss << "ecom-txn-" << ms << "-" << r;
    return oss.str();
}

json processECOMTransaction(const json &data) {
    json res;

    try {
        std::string clientTxnId = data.value("clientTxnId", genTxnId());
        std::string txnType     = data.value("transactionType", "PURCHASE");
        std::string merchantId  = data.value("merchantId", "");
        std::string orderId     = data.value("orderId", "");
        double amount           = data.value("amount", 0.0);
        double fee              = data.value("fee", 0.0);
        std::string currency    = data.value("currency", "USD");

        if (!data.contains("card")) {
            res["errorCode"] = "ERR_MISSING_CARD";
            res["message"]   = "card object required";
            return res;
        }

        json card      = data["card"];
        std::string pan    = card.value("pan", "");
        std::string expiry = card.value("expiry", "");
        std::string cvv    = card.value("cvv", "");

        if (pan.empty() || expiry.empty()) {
            res["errorCode"] = "ERR_CARD_INCOMPLETE";
            res["message"]   = "card.pan & card.expiry required";
            return res;
        }

        // DB Session
        Session sess("localhost", 33060, "root", "Rohan@5649");
        Schema db = sess.getSchema("bankingdb");

        Table cards = db.getTable("cards");
        Table accounts = db.getTable("accounts");
        Table ecom = db.getTable("transaction_ecom");
        Table master = db.getTable("transactions");

        // Get card
        RowResult cardRes = cards
            .select("account_number","scheme","expiry","cvv","status")
            .where("pan = :p AND expiry = :e")
            .bind("p", pan)
            .bind("e", expiry)
            .execute();

        if (cardRes.count() == 0) {
            res["errorCode"] = "ERR_CARD_NOT_FOUND";
            res["message"] = "Invalid PAN or expiry";
            return res;
        }

        Row cardRow = cardRes.fetchOne();
        std::string accNo      = cardRow[0].get<std::string>();
        std::string cardScheme = cardRow[1].isNull() ? "" : cardRow[1].get<std::string>();
        std::string dbCvv      = cardRow[3].isNull() ? "" : cardRow[3].get<std::string>();
        std::string status     = cardRow[4].get<std::string>();

        if (status != "ACTIVE") {
            res["errorCode"] = "ERR_CARD_BLOCKED";
            res["message"]   = "Card is not active";
            return res;
        }

        if (!cvv.empty() && cvv != dbCvv) {
            res["errorCode"] = "ERR_CVV_MISMATCH";
            res["message"]   = "Invalid CVV";
            return res;
        }

        // Account
        RowResult accRes = accounts
            .select("currency","balance")
            .where("account_number = :a")
            .bind("a", accNo)
            .execute();

        if (accRes.count() == 0) {
            res["errorCode"] = "ERR_ACC_NOT_FOUND";
            res["message"]   = "Linked account missing";
            return res;
        }

        Row accRow = accRes.fetchOne();
        std::string accCurrency = accRow[0].get<std::string>();
        double balance = accRow[1].get<double>();

        std::string scope = (currency == accCurrency) ? "DOMESTIC" : "INTERNATIONAL";
        std::string txnId = genTxnId();

        // ---------------- PURCHASE ----------------
        if (txnType == "PURCHASE") {

            if (balance < amount + fee) {
                res["errorCode"] = "ERR_NO_FUNDS";
                res["message"]   = "Insufficient balance";
                return res;
            }

            double newBal = balance - (amount + fee);

            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a").bind("a", accNo).execute();

            auto ins = ecom.insert(
                    "transaction_id","client_txn_id","merchant_id",
                    "order_id","currency","transaction_scope","account_number",
                    "amount","fee","card_pan","card_scheme","status","message"
                )
                .values(
                    txnId, clientTxnId, merchantId, orderId, currency,
                    scope, accNo, amount, fee, pan, cardScheme,
                    "SUCCESS", "E-commerce purchase successful"
                )
                .execute();

            master.insert("table_name","reference_id","status")
                .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
                .execute();

            res["txnId"] = txnId;
            res["status"] = "SUCCESS";
            res["balanceAfter"] = newBal;
            return res;
        }

        // ---------------- REFUND ----------------
        if (txnType == "REFUND") {

            // Find Purchase
            RowResult pRes = ecom
                .select("id","amount","card_pan")
                .where("order_id = :o AND merchant_id = :m AND message = 'E-commerce purchase successful'")
                .bind("o", orderId)
                .bind("m", merchantId)
                .execute();

            if (pRes.count() == 0) {
                res["errorCode"] = "ERR_PURCHASE_NOT_FOUND";
                res["message"]   = "Purchase not found";
                return res;
            }

            Row p = pRes.fetchOne();
            int64_t purchaseId = p[0].get<int64_t>();
            double purchaseAmt = p[1].get<double>();
            std::string purchasePan = p[2].get<std::string>();

            if (purchasePan != pan) {
                res["errorCode"] = "ERR_PAN_MISMATCH";
                res["message"]   = "Refund PAN does not match purchase";
                return res;
            }

            // SUM refunds
            SqlStatement stmt = sess.sql(
                "SELECT COALESCE(SUM(amount),0) "
                "FROM bankingdb.transaction_ecom "
                "WHERE order_id = ? AND merchant_id = ? AND message = 'Refund successful'"
            );
            stmt.bind(orderId);
            stmt.bind(merchantId);

            Row r = stmt.execute().fetchOne();
            double refundedSoFar = r[0].isNull() ? 0.0 : r[0].get<double>();

            if (refundedSoFar >= purchaseAmt) {
                res["errorCode"] = "ERR_FULLY_REFUNDED";
                res["message"]   = "Already refunded fully";
                return res;
            }

            if (refundedSoFar + amount > purchaseAmt) {
                res["errorCode"] = "ERR_REFUND_EXCEEDS";
                res["message"]   = "Refund exceeds purchase";
                return res;
            }

            // Apply refund
            double newBal = balance + amount;

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
                txnId, clientTxnId, merchantId, orderId, currency,
                scope, accNo, amount, 0.0, pan, cardScheme,
                "SUCCESS", "Refund successful"
            )
            .execute();

            master.insert("table_name","reference_id","status")
                .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
                .execute();

            res["txnId"] = txnId;
            res["status"] = "SUCCESS";
            res["refundedSoFar"] = refundedSoFar + amount;
            return res;
        }

        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"] = "Invalid ECOM type";
        return res;
    }
    catch (const std::exception &e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"] = e.what();
        return res;
    }
}
