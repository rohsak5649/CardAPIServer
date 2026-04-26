/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* QR CODE PAYMENT PROCESSING — THREAD-SAFE v2.1
*
* KEY CHANGE: Database::getSession() → Database::ScopedConnection
*   Each concurrent QR request gets its own dedicated DB session.
*
* For implementation details, contact: +91 9112765649
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
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "qr-txn-" << ms << "-" << (std::rand() % 9000 + 1000);
    return oss.str();
}

static std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

static std::string find_kv(const std::string& qrData, const std::string& key) {
    std::string data = qrData;
    for (char& c : data) if (c == '&' || c == '|') c = ';';
    std::string lowerData = lower(data);
    std::string lowerKey  = lower(key) + "=";
    size_t pos = lowerData.find(lowerKey);
    if (pos == std::string::npos) return "";
    pos += lowerKey.size();
    size_t end = lowerData.find(';', pos);
    if (end == std::string::npos) end = data.size();
    std::string val = data.substr(pos, end-pos);
    size_t a = val.find_first_not_of(" \t\n\r");
    size_t b = val.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    return val.substr(a, b-a+1);
}

json processQRCodePayment(const json& data) {
    json res;

    // ★ Each request acquires its OWN session from the pool
    Database::ScopedConnection sc;
    Session& sess = *sc;
    Schema db = sess.getSchema("bankingdb");

    try {
        std::string clientTxnId  = data.value("clientTxnId", genQRTxnId());
        std::string txnType      = data.value("transactionType", std::string("PURCHASE"));
        std::string qrData       = data.value("qrData", std::string());
        std::string merchantName = data.value("merchantName", std::string());
        std::string mid          = data.value("mid", std::string());
        std::string tid          = data.value("tid", std::string());
        double amount            = data.value("amount", 0.0);
        double fee               = data.value("fee", 0.0);
        std::string currency     = data.value("currency", std::string("AUD"));
        std::string accountNumber = data.value("accountNumber", std::string());

        if (!qrData.empty()) {
            std::string kv;
            kv = find_kv(qrData,"mid");          if (!kv.empty()) mid = kv;
            kv = find_kv(qrData,"merchantName"); if (!kv.empty()) merchantName = kv;
            kv = find_kv(qrData,"tid");          if (!kv.empty()) tid = kv;
            kv = find_kv(qrData,"currency");     if (!kv.empty()) currency = kv;
            kv = find_kv(qrData,"amount");
            if (!kv.empty()) try { amount = std::stod(kv); } catch (...) {}
        }

        if (txnType == "PURCHASE" && amount <= 0.0) {
            res["errorCode"] = "ERR_INVALID_AMOUNT";
            res["message"]   = "Purchase amount must be > 0.";
            return res;
        }
        if (accountNumber.empty()) {
            res["errorCode"] = "ERR_MISSING_ACCOUNT";
            res["message"]   = "accountNumber is required.";
            return res;
        }

        Table accounts = db.getTable("accounts");
        Table qrTable  = db.getTable("transaction_qrcode");
        Table master   = db.getTable("transactions");

        RowResult accRes = accounts.select("currency","balance")
            .where("account_number = :a").bind("a",accountNumber).execute();
        if (accRes.count() == 0) {
            res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            res["message"]   = "Debit account not found.";
            return res;
        }

        Row accRow = accRes.fetchOne();
        std::string accCurrency = accRow[0].get<std::string>();
        double balance = accRow[1].get<double>();
        std::string txnId = genQRTxnId();
        std::string scope = (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

        // ── PURCHASE ──────────────────────────────────────────────────────────
        if (txnType == "PURCHASE") {
            if (balance < amount + fee) {
                res["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
                res["message"]   = "Insufficient balance.";
                return res;
            }
            double newBal = balance - (amount + fee);
            accounts.update().set("balance",newBal)
                .where("account_number = :a").bind("a",accountNumber).execute();

            auto ins = qrTable.insert(
                "transaction_id","client_txn_id","qr_raw_data","merchant_name",
                "merchant_id","terminal_id","account_number","amount","fee",
                "currency","transaction_scope","status","message")
                .values(txnId,clientTxnId,qrData,merchantName,
                        mid,tid,accountNumber,amount,fee,
                        currency,scope,"SUCCESS","QR purchase successful")
                .execute();

            master.insert("table_name","reference_id","status")
                .values("transaction_qrcode", ins.getAutoIncrementValue(), "SUCCESS").execute();

            res["transactionId"]   = txnId;
            res["status"]          = "SUCCESS";
            res["balanceAfter"]    = newBal;
            res["merchantName"]    = merchantName;
            res["merchantId"]      = mid;
            res["transactionScope"]= scope;
            return res;
        }

        // ── REFUND ────────────────────────────────────────────────────────────
        if (txnType == "REFUND") {
            if (!data.contains("origClientTxnId")) {
                res["errorCode"] = "ERR_MISSING_ORIG_REF";
                res["message"]   = "origClientTxnId required for refund.";
                return res;
            }
            std::string origClient = data["origClientTxnId"].get<std::string>();

            RowResult pRes = qrTable.select("id","amount","account_number")
                .where("client_txn_id = :cid AND message = 'QR purchase successful'")
                .bind("cid",origClient).execute();
            if (pRes.count() == 0) { res["errorCode"]="ERR_PURCHASE_NOT_FOUND"; return res; }

            Row pRow = pRes.fetchOne();
            int64_t purchaseId  = pRow[0].get<int64_t>();
            double purchaseAmt  = pRow[1].get<double>();
            std::string purchAcc = pRow[2].get<std::string>();

            if (purchAcc != accountNumber) { res["errorCode"]="ERR_ACCOUNT_MISMATCH"; return res; }

            RowResult already = qrTable.select("id")
                .where("orig_ref_id = :pid AND message = 'QR refund successful'")
                .bind("pid",purchaseId).execute();
            if (already.count() > 0) { res["errorCode"]="ERR_ALREADY_REFUNDED"; return res; }

            if (amount > purchaseAmt) { res["errorCode"]="ERR_REFUND_EXCEEDS"; return res; }

            double newBal = balance + amount;
            accounts.update().set("balance",newBal)
                .where("account_number = :a").bind("a",accountNumber).execute();

            auto ins = qrTable.insert(
                "transaction_id","client_txn_id","qr_raw_data","merchant_name",
                "merchant_id","terminal_id","account_number","amount","fee",
                "currency","transaction_scope","status","message","orig_ref_id")
                .values(txnId,clientTxnId,qrData,merchantName,
                        mid,tid,accountNumber,amount,0.0,
                        currency,scope,"SUCCESS","QR refund successful",purchaseId)
                .execute();

            master.insert("table_name","reference_id","status")
                .values("transaction_qrcode", ins.getAutoIncrementValue(), "SUCCESS").execute();

            res["transactionId"] = txnId;
            res["status"]        = "SUCCESS";
            res["balanceAfter"]  = newBal;
            res["purchaseAmount"]= purchaseAmt;
            res["refundedAmount"]= amount;
            return res;
        }

        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"]   = "Unsupported QR transactionType";
        return res;

    } catch (const std::exception& e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"]   = e.what();
        return res;
    }
}