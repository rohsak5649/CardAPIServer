/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*/

#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <sstream>
#include <random>
#include <future>   // ✅ NEW
#include "falcon.h"
#include "Database.h"
#include "pin.h"

using namespace mysqlx;
using json = nlohmann::json;

// Generate unique Mobile Transaction ID
std::string generateMobileTxnId() {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    int r = rand() % 9000 + 1000;
    std::ostringstream oss;
    oss << "mobile-txn-" << ms << "-" << r;
    return oss.str();
}

// ================= CORE FUNCTION (ORIGINAL + SAFE ADDITIONS) =================
json processMobileTransactionCore(const json &data) {
    json response;

    try {
        std::string clientTxnId = data["clientTxnId"];
        std::string txnType     = data["transactionType"];

        // ✅ KEEP EXISTING (fallback)
        std::string debitAcc = "";
        if (data.contains("debitAccount"))
            debitAcc = data["debitAccount"]["accountNumber"];

        std::string creditAcc   = data["creditAccount"]["accountNumber"];
        double amount           = data["amount"];
        double fee              = data["fee"];
        std::string deviceId    = data["deviceId"];
        std::string mobileNo    = data["mobileNumber"];

        Session& sess = Database::getSession();
        Schema db = Database::getSchema();

        Table accounts     = db.getTable("accounts");
        Table cards        = db.getTable("cards");
        Table mobileTable  = db.getTable("transaction_mobile");
        Table masterTable  = db.getTable("transactions");

        sess.startTransaction();

        // ================== ✅ NEW: PAN FLOW (NON-BREAKING) ==================
        if (data.contains("pan") && data.contains("pin")) {

            std::string inputPan = data["pan"];
            std::string inputPin = data["pin"];

            if (inputPan.length() != 16) {
                response["errorCode"] = "ERR_INVALID_PAN";
                response["message"] = "Invalid PAN format.";
                sess.rollback();
                return response;
            }

            // Fetch account from PAN
            RowResult accFetch = cards.select("account_number")
                .where("pan = :pan AND card_priority = 'PRIMARY'")
                .bind("pan", inputPan)
                .execute();

            if (accFetch.count() == 0) {
                response["errorCode"] = "ERR_CARD_NOT_FOUND";
                response["message"] = "Invalid PAN.";
                sess.rollback();
                return response;
            }

            // ✅ override debit account
            debitAcc = accFetch.fetchOne()[0].get<std::string>();

            // PIN verification
            try {
                auto& pinService = PINService::getInstance();

                if (!pinService.verifyPIN(inputPan, inputPin)) {
                    response["errorCode"] = "ERR_INVALID_PIN";
                    response["message"] = "PIN verification failed.";
                    sess.rollback();
                    return response;
                }
            }
            catch (const std::exception& e) {
                response["errorCode"] = "ERR_PIN_SYSTEM";
                response["message"] = e.what();
                sess.rollback();
                return response;
            }
        }

        // ================== EXISTING FALCON LOGIC ==================
        Falcon falcon(sess);

        std::string fraudReason;
        bool isFraud = falcon.checkFraud(debitAcc, amount, fraudReason);

        if (isFraud) {
            std::string txnId = generateMobileTxnId();

            falcon.logFraud(
                txnId,
                clientTxnId,
                deviceId,
                mobileNo,
                debitAcc,
                amount,
                fraudReason
            );

            sess.commit();

            response["transactionId"] = txnId;
            response["status"] = "DECLINED";
            response["message"] = fraudReason;
            return response;
        }

        // ------------------ Validate type ------------------
        if (txnType != "FUND_TRANSFER") {
            response["errorCode"] = "ERR_INVALID_TYPE";
            response["message"] = "Invalid MOBILE transaction type.";
            sess.rollback();
            return response;
        }

        // ------------------ One-time limit ------------------
        if (amount > 2000.0) {
            response["errorCode"] = "ERR_ONE_TIME_LIMIT";
            response["message"] = "Single transaction cannot exceed ₹2,000.";
            sess.rollback();
            return response;
        }

        // ------------------ Rolling 1-hour limit ------------------
        RowResult hourlyRes = sess.sql(
            "SELECT SUM(amount) FROM bankingdb.transaction_mobile "
            "WHERE account_number = ? "
            "AND status = 'SUCCESS' "
            "AND created_at >= NOW() - INTERVAL 1 HOUR"
        )
        .bind(debitAcc)
        .execute();

        double hourlyTotal = 0.0;

        if (hourlyRes.count() > 0) {
            Row row = hourlyRes.fetchOne();
            if (!row[0].isNull())
                hourlyTotal = row[0].get<double>();
        }

        double hourlyLimit = 2000.0;
        double remainingLimit = hourlyLimit - hourlyTotal;

        if ((hourlyTotal + amount) > hourlyLimit) {
            response["errorCode"] = "ERR_HOURLY_LIMIT";

            std::ostringstream msg;
            msg << "Hourly limit exceeded. You can transact only ₹"
                << remainingLimit << " more in this hour.";

            response["message"] = msg.str();
            response["remainingLimit"] = remainingLimit;

            sess.rollback();
            return response;
        }

        // ------------------ Daily limit ------------------
        RowResult dailyRes = mobileTable.select("SUM(amount)")
            .where("account_number = :acc AND status = 'SUCCESS' AND DATE(created_at) = CURDATE()")
            .bind("acc", debitAcc)
            .execute();

        double dailyTotal = 0.0;
        if (dailyRes.count() > 0) {
            Row row = dailyRes.fetchOne();
            if (!row[0].isNull())
                dailyTotal = row[0].get<double>();
        }

        if ((dailyTotal + amount) > 10000.0) {
            response["errorCode"] = "ERR_DAILY_LIMIT";
            response["message"] = "Daily limit of ₹10,000 exceeded.";
            sess.rollback();
            return response;
        }

        // ------------------ Validate debit account ------------------
        RowResult debitRes = accounts.select("balance")
            .where("account_number = :acc")
            .bind("acc", debitAcc)
            .execute();

        if (debitRes.count() == 0) {
            response["errorCode"] = "ERR_DEBIT_ACCOUNT_NOT_FOUND";
            response["message"] = "Debit account not found.";
            sess.rollback();
            return response;
        }

        Row debitRow = debitRes.fetchOne();
        double debitPrev = debitRow[0].get<double>();

        // ------------------ Validate credit account ------------------
        RowResult creditRes = accounts.select("balance")
            .where("account_number = :acc")
            .bind("acc", creditAcc)
            .execute();

        if (creditRes.count() == 0) {
            response["errorCode"] = "ERR_CREDIT_ACCOUNT_NOT_FOUND";
            response["message"] = "Credit account not found.";
            sess.rollback();
            return response;
        }

        Row creditRow = creditRes.fetchOne();
        double creditPrev = creditRow[0].get<double>();

        // ------------------ Balance check ------------------
        if (debitPrev < (amount + fee)) {
            response["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
            response["message"] = "Insufficient balance.";
            sess.rollback();
            return response;
        }

        // ------------------ Update balances ------------------
        double debitNew  = debitPrev - (amount + fee);
        double creditNew = creditPrev + amount;

        accounts.update()
            .set("balance", debitNew)
            .where("account_number = :acc")
            .bind("acc", debitAcc)
            .execute();

        accounts.update()
            .set("balance", creditNew)
            .where("account_number = :acc")
            .bind("acc", creditAcc)
            .execute();

        // ------------------ Insert transaction ------------------
        std::string txnId = generateMobileTxnId();

        auto mobileInsert = mobileTable.insert(
            "transaction_id","client_txn_id","device_id","mobile_number",
            "account_number","amount","fee","status","message"
        )
        .values(
            txnId, clientTxnId, deviceId, mobileNo,
            debitAcc, amount, fee,
            "SUCCESS", "Mobile fund transfer successful"
        )
        .execute();

        long childId = mobileInsert.getAutoIncrementValue();

        masterTable.insert("table_name","reference_id","status")
            .values("transaction_mobile", childId, "SUCCESS")
            .execute();

        sess.commit();

        response["transactionId"] = txnId;
        response["status"] = "SUCCESS";
        response["balanceAfterDebit"] = debitNew;

        return response;
    }
    catch (const std::exception &e) {
        response["errorCode"] = "ERR_EXCEPTION";
        response["message"] = e.what();
        return response;
    }
}

// ================== TIMEOUT WRAPPER ==================
json processMobileTransaction(const json &data) {

    auto future = std::async(std::launch::async, processMobileTransactionCore, data);

    if (future.wait_for(std::chrono::seconds(6)) == std::future_status::timeout) {
        json response;
        response["status"] = "DECLINED";
        response["errorCode"] = "ERR_TIMEOUT";
        response["message"] = "Transaction timeout (more than 6 seconds)";
        return response;
    }

    return future.get();
}