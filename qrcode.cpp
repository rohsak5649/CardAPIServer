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
#include "TransactionLogger.h"
#include "AccountLockManager.h"
#include "global_contant.h"
#include "DatabaseQueries.h"
#include "falcon.h"

using namespace mysqlx;
using json = nlohmann::json;


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
    TransactionLogger::ScopedFunctionTrace trace("processQRCodePayment",
                                                 {{"transactionType", data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE))}});
    std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
    TransactionLogger::ScopedContext scope(uuid, "QRCODE");
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_started", "QR Code transaction handler started",
        {{"transactionType", data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE))}});

    // ★ Each request acquires its OWN session from the pool
    Database::ScopedConnection sc;
    Session& sess = *sc;
    Schema db = sess.getSchema("bankingdb");

    try {
        std::string clientTxnId  = data.value("clientTxnId", TransactionLogger::currentUuid());

        TransactionType txnType  = stringToTransactionType(data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE)));
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

        if (txnType == TransactionType::PURCHASE && amount <= 0.0) {
            res["errorCode"] = "ERR_INVALID_AMOUNT";
            res["message"]   = "Purchase amount must be > 0.";
            trace.fail("invalid QR purchase amount", {{"amount", std::to_string(amount)}});
            return res;
        }
        if (accountNumber.empty()) {
            res["errorCode"] = "ERR_MISSING_ACCOUNT";
            res["message"]   = "accountNumber is required.";
            trace.fail("missing account number");
            return res;
        }

        Table accounts = db.getTable("accounts");
        Table qrTable  = db.getTable("transaction_qrcode");
        Table master   = db.getTable("transactions");

        // accounts table has currency_id (FK), not currency column — must JOIN
        auto accInfo = DatabaseQueries::getAccountBalanceAndCurrency(sess, accountNumber);
        if (!accInfo) {
            res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            res["message"]   = "Debit account not found.";
            trace.fail("account not found", {{"accountNumber", accountNumber}});
            return res;
        }

        double balance = accInfo->balance;
        std::string accCurrency = accInfo->currencyCode.empty() ? "AUD" : accInfo->currencyCode;
        // Use the correlation UUID injected by the router as the DB transaction_id.
        // Eliminates the std::rand() data race and the timestamp collision risk.
        std::string txnId = data.value("_correlationUuid", TransactionLogger::currentUuid());
        std::string scope = (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

        // ── Falcon fraud and AI security check ───────────────────────────────
        Falcon falcon(sess);
        std::string fraudReason;
        if (falcon.checkFraud(accountNumber, amount, fraudReason,
                              FalconChannel::QRCODE, &data)) {
            falcon.logFraud(txnId, clientTxnId, tid, "", accountNumber,
                            amount, fraudReason);
            res["transactionId"] = txnId;
            res["status"] = "DECLINED";
            res["errorCode"] = "ERR_FRAUD";
            res["message"] = fraudReason;
            trace.fail("fraud declined QR transaction", {{"reason", fraudReason}});
            return res;
        }

        // ── PURCHASE ──────────────────────────────────────────────────────────
        if (txnType == TransactionType::PURCHASE) {
            AccountLockManager::ScopedLock accLock(
                AccountLockManager::getInstance(), accountNumber, TxnPriority::DEBIT);
            sess.startTransaction();
            try {
                auto currentBalOpt = DatabaseQueries::getAccountBalance(sess, accountNumber, true);
                if (!currentBalOpt) {
                    sess.rollback();
                    res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
                    res["message"]   = "Debit account not found.";
                    trace.fail("account not found", {{"accountNumber", accountNumber}});
                    return res;
                }
                double currentBal = *currentBalOpt;

                if (currentBal < amount + fee) {
                    sess.rollback();
                    res["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
                    res["message"]   = "Insufficient balance.";
                    trace.fail("insufficient balance",
                               {{"balance", std::to_string(currentBal)},
                                {"amount", std::to_string(amount)},
                                {"fee", std::to_string(fee)}});
                    return res;
                }
                double newBal = currentBal - (amount + fee);
                DatabaseQueries::updateAccountBalance(sess, accountNumber, newBal);

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

                sess.commit();

                res["transactionId"]   = txnId;
                res["status"]          = "SUCCESS";
                res["balanceAfter"]    = newBal;
                res["merchantName"]    = merchantName;
                res["merchantId"]      = mid;
                res["transactionScope"]= scope;
                trace.success({{"transactionId", txnId},
                               {"merchantId", mid},
                               {"scope", scope}});
                return res;
            } catch (...) {
                sess.rollback();
                throw;
            }
        }

        // ── REFUND ────────────────────────────────────────────────────────────
        if (txnType == TransactionType::REFUND) {
            if (!data.contains("origClientTxnId")) {
                res["errorCode"] = "ERR_MISSING_ORIG_REF";
                res["message"]   = "origClientTxnId required for refund.";
                trace.fail("missing original client transaction id");
                return res;
            }
            std::string origClient = data["origClientTxnId"].get<std::string>();

            AccountLockManager::ScopedLock accLock(
                AccountLockManager::getInstance(), accountNumber, TxnPriority::CREDIT);

            sess.startTransaction();
            try {
                // Lock original transaction row using FOR UPDATE to prevent concurrent refunding
                auto pInfo = DatabaseQueries::getQrPurchaseForUpdate(sess, origClient);
                if (!pInfo) {
                    sess.rollback();
                    res["errorCode"]="ERR_PURCHASE_NOT_FOUND";
                    trace.fail("original QR purchase not found", {{"origClientTxnId", origClient}});
                    return res;
                }

                int64_t purchaseId  = pInfo->id;
                double purchaseAmt  = pInfo->amount;
                std::string purchAcc = pInfo->accountNumber;

                if (purchAcc != accountNumber) {
                    sess.rollback();
                    res["errorCode"]="ERR_ACCOUNT_MISMATCH";
                    trace.fail("refund account mismatch");
                    return res;
                }

                // Check if already refunded (FOR UPDATE)
                if (DatabaseQueries::checkQrAlreadyRefunded(sess, purchaseId)) {
                    sess.rollback();
                    res["errorCode"]="ERR_ALREADY_REFUNDED";
                    trace.fail("QR purchase already refunded");
                    return res;
                }

                if (amount > purchaseAmt) {
                    sess.rollback();
                    res["errorCode"]="ERR_REFUND_EXCEEDS";
                    trace.fail("QR refund exceeds purchase amount",
                               {{"amount", std::to_string(amount)},
                                {"purchaseAmount", std::to_string(purchaseAmt)}});
                    return res;
                }

                // Lock account balance (FOR UPDATE)
                auto balRes = DatabaseQueries::getAccountBalance(sess, accountNumber, true);
                if (!balRes) {
                    sess.rollback();
                    res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
                    trace.fail("account not found", {{"accountNumber", accountNumber}});
                    return res;
                }
                double currentBal = *balRes;

                double newBal = currentBal + amount;
                DatabaseQueries::updateAccountBalance(sess, accountNumber, newBal);

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

                sess.commit();

                res["transactionId"] = txnId;
                res["status"]        = "SUCCESS";
                res["balanceAfter"]  = newBal;
                res["purchaseAmount"]= purchaseAmt;
                res["refundedAmount"]= amount;
                trace.success({{"transactionId", txnId},
                               {"refundedAmount", std::to_string(amount)}});
                return res;
            } catch (...) {
                sess.rollback();
                throw;
            }
        }

        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"]   = "Unsupported QR transactionType";
        trace.fail("unsupported QR transaction type", {{"transactionType", transactionTypeToString(txnType)}});
        return res;

    } catch (const std::exception& e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"]   = e.what();
        trace.fail("QR code payment exception", {{"error", e.what()}});
        return res;
    }
}
