#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <sstream>
#include <random>

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

json processMobileTransaction(const json &data) {
    json response;

    try {
        // Extract fields
        std::string clientTxnId = data["clientTxnId"];
        std::string txnType     = data["transactionType"];   // FUND_TRANSFER
        std::string debitAcc    = data["debitAccount"]["accountNumber"];
        std::string creditAcc   = data["creditAccount"]["accountNumber"];
        double amount           = data["amount"];
        double fee              = data["fee"];
        std::string deviceId    = data["deviceId"];
        std::string mobileNo    = data["mobileNumber"];

        Session sess("localhost", 33060, "root", "Rohan@5649");
        Schema db = sess.getSchema("bankingdb");

        Table accounts     = db.getTable("accounts");
        Table cards        = db.getTable("cards");
        Table mobileTable  = db.getTable("transaction_mobile");
        Table masterTable  = db.getTable("transactions");

        // ------------------ Validate debit account ------------------
        RowResult debitRes = accounts.select("balance", "country_code")
            .where("account_number = :acc")
            .bind("acc", debitAcc)
            .execute();

        if (debitRes.count() == 0) {
            response["errorCode"] = "ERR_DEBIT_ACCOUNT_NOT_FOUND";
            response["message"] = "Debit account not found.";
            return response;
        }

        Row debitRow = debitRes.fetchOne();
        double debitPrev = debitRow[0].get<double>();
        std::string debitCountry = debitRow[1].get<std::string>();

        // ------------------ Validate credit account ------------------
        RowResult creditRes = accounts.select("balance", "country_code")
            .where("account_number = :acc")
            .bind("acc", creditAcc)
            .execute();

        if (creditRes.count() == 0) {
            response["errorCode"] = "ERR_CREDIT_ACCOUNT_NOT_FOUND";
            response["message"] = "Recipient account not found.";
            return response;
        }

        Row creditRow = creditRes.fetchOne();
        double creditPrev = creditRow[0].get<double>();
        std::string creditCountry = creditRow[1].get<std::string>();

        // Domestic or International
        std::string txnScope = (debitCountry == creditCountry) ? "DOMESTIC" : "INTERNATIONAL";

        // ------------------ Validate type ------------------
        if (txnType != "FUND_TRANSFER") {
            response["errorCode"] = "ERR_INVALID_TYPE";
            response["message"] = "Invalid MOBILE transaction type.";
            return response;
        }

        // ------------------ Check balance ------------------
        if (debitPrev < (amount + fee)) {
            response["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
            response["message"] = "Insufficient balance.";
            return response;
        }

        // ------------------ Fetch PRIMARY CARD ------------------
        RowResult cardRes = cards.select("pan", "scheme")
            .where("account_number = :acc AND card_priority = 'PRIMARY'")
            .bind("acc", debitAcc)
            .execute();

        if (cardRes.count() == 0) {
            response["errorCode"] = "ERR_NO_PRIMARY_CARD";
            response["message"] = "No PRIMARY card found for this account.";
            return response;
        }

        Row cardRow = cardRes.fetchOne();
        std::string primaryPan    = cardRow[0].get<std::string>();
        std::string primaryScheme = cardRow[1].get<std::string>();

        // ------------------ Balance updates ------------------
        double debitNew = debitPrev - (amount + fee);
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

        // Generate TXN ID
        std::string txnId = generateMobileTxnId();

        // ------------------ Insert into transaction_mobile ------------------
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

        // ------------------ Insert into MASTER table ------------------
        masterTable.insert("table_name","reference_id","status")
            .values("transaction_mobile", childId, "SUCCESS")
            .execute();

        // ------------------ Prepare Response ------------------
        response["transactionId"]     = txnId;
        response["status"]            = "SUCCESS";
        response["debitedFrom"]       = debitAcc;
        response["creditedTo"]        = creditAcc;
        response["primaryCardUsed"]   = primaryPan;
        response["primaryScheme"]     = primaryScheme;
        response["transactionScope"]  = txnScope;
        response["balanceAfterDebit"] = debitNew;

        sess.close();
        return response;
    }
    catch (const std::exception &e) {
        response["errorCode"] = "ERR_EXCEPTION";
        response["message"] = e.what();
        return response;
    }
}
