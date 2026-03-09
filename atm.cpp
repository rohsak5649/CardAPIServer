#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>
#include "Database.h"

using namespace mysqlx;
using json = nlohmann::json;

// Utility: Generate unique transaction ID
static std::string generateTxnId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> d(1000,9999);
    std::ostringstream oss;
    oss << "atm-txn-" << ms << "-" << d(gen);
    return oss.str();
}

json processATMTransaction(const json& data) {
    json response;

    try {
        // Extract fields
        std::string clientTxnId   = data["clientTxnId"];
        std::string txnType       = data["transactionType"];
        std::string atmId         = data["atmId"];
        std::string terminalId    = data["terminalId"];
        std::string location      = data["location"];
        std::string pan           = data["card"]["pan"];
        std::string expiry        = data["card"]["expiry"];
        std::string accountNumber = data["debitAccount"]["accountNumber"];
        double amount             = data["amount"];
        double fee                = data["fee"];

        Session& sess = Database::getSession();
        Schema db = Database::getSchema();

        Table cards = db.getTable("cards");
        Table accounts = db.getTable("accounts");
        Table atmTable = db.getTable("transaction_atm");
        Table masterTable = db.getTable("transactions");

        // Validate card
        RowResult cardRes = cards.select("account_number", "status", "scheme")
                                .where("pan = :p AND expiry = :e")
                                .bind("p", pan)
                                .bind("e", expiry)
                                .execute();

        if (cardRes.count() == 0) {
            response["errorCode"] = "ERR_INVALID_CARD";
            response["message"] = "Card not found or expired.";
            return response;
        }

        Row cardRow = cardRes.fetchOne();
        std::string dbAcc = cardRow[0].get<std::string>();
        std::string cardStatus = cardRow[1].get<std::string>();
        std::string cardScheme = cardRow[2].get<std::string>();

        if (cardStatus != "ACTIVE") {
            response["errorCode"] = "ERR_CARD_INACTIVE";
            response["message"] = "Card is inactive.";
            return response;
        }

        // Validate account
        RowResult accRes = accounts.select("balance")
                                   .where("account_number = :a")
                                   .bind("a", dbAcc)
                                   .execute();

        if (accRes.count() == 0) {
            response["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            response["message"] = "Account does not exist.";
            return response;
        }

        double prevBal = accRes.fetchOne()[0].get<double>();
        double newBal = prevBal;

        std::string txnId = generateTxnId();
        std::string status = "SUCCESS";
        std::string msg = "";

        // ----------------------- WITHDRAWAL -------------------------
        if (txnType == "WITHDRAWAL") {
            if (prevBal < (amount + fee)) {
                response["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
                response["message"] = "Not enough balance.";
                return response;
            }

            newBal = prevBal - (amount + fee);

            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", dbAcc)
                .execute();

            msg = "ATM withdrawal successful.";
        }

        // -------------------------- DEPOSIT --------------------------
        else if (txnType == "DEPOSIT") {
            newBal = prevBal + amount;

            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", dbAcc)
                .execute();

            msg = "ATM deposit successful.";
        }

        else {
            response["errorCode"] = "ERR_INVALID_TYPE";
            response["message"] = "Unsupported ATM transaction type.";
            return response;
        }

        // ---------------- INSERT into transaction_atm ----------------
        auto atmResult = atmTable.insert(
            "transaction_id","client_txn_id","atm_id","terminal_id",
            "location","account_number","amount","fee",
            "card_pan","card_scheme","status","message"
        )
        .values(
            txnId, clientTxnId, atmId, terminalId,
            location, dbAcc, amount, fee,
            pan, cardScheme, "SUCCESS", msg
        )
        .execute();

        long childId = atmResult.getAutoIncrementValue();

        // ---------------- INSERT into MASTER transactions ------------
        masterTable.insert("table_name","reference_id","status")
                   .values("transaction_atm", childId, "SUCCESS")
                   .execute();

        // ---------------- HTTP JSON Response ----------------
        response["transactionId"] = txnId;
        response["status"] = "SUCCESS";
        response["message"] = msg;
        response["balanceAfter"] = newBal;

        sess.close();
        return response;
    }
    catch (const std::exception &e) {
        response["errorCode"] = "ERR_EXCEPTION";
        response["message"] = e.what();
        return response;
    }
}
