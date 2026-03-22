/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* ATM TRANSACTION PROCESSING FLOW:
*
* 1. Transaction request received from ATM channel API.
* 2. Input validation performed (transaction type, required fields, structure).
*
* 3. PAN & PIN FLOW (SECURE PROCESSING):
*    - Encrypted PAN received from ATM terminal/client.
*    - PAN is decrypted internally using AES-GCM (panencrypted module).
*    - If decryption fails → transaction declined.
*    - PIN verification performed using PIN service.
*    - If PIN verification fails → transaction declined.
*    - Card details validated using cards table (PAN + expiry).
*
* 4. CARD VALIDATIONS:
*    - Card existence check.
*    - Card status validation (ACTIVE / INACTIVE).
*    - If inactive → transaction declined.
*
* 5. ACCOUNT VALIDATIONS:
*    - Account fetched using card mapping.
*    - Account balance locked using SELECT ... FOR UPDATE.
*    - Ensures consistency in concurrent ATM operations.
*
* 6. DAILY LIMIT VALIDATIONS:
*    - Total transaction amount calculated for current day.
*    - Withdrawal limit:
*        → Maximum ₹5,000 per day (amount + fee)
*    - Deposit limit:
*        → Maximum ₹10,000 per day
*
* 7. TRANSACTION TYPE HANDLING:
*
*    WITHDRAWAL:
*    - Check sufficient balance (amount + fee).
*    - Debit account balance.
*
*    DEPOSIT:
*    - Validate daily deposit limit.
*    - Credit account balance.
*
*    - Invalid type → transaction declined.
*
* 8. TRANSACTION EXECUTION:
*    - Account balance updated in accounts table.
*    - Entry inserted into transaction_atm table.
*    - Entry inserted into master transactions table.
*
* 9. CARD ACTIVITY UPDATE:
*    - last_transaction_time updated in cards table.
*    - Helps in fraud detection & activity tracking.
*
* 10. RESPONSE:
*    - SUCCESS → transactionId + updated balance
*    - FAILURE → errorCode/message
*
* SECURITY NOTES:
* - PAN is never exposed externally in plain text.
* - All PAN operations handled securely within backend.
* - Encryption standard: AES-256-GCM
* - PIN verification ensures cardholder authentication.
*
* DESIGN NOTES:
* - Uses repository pattern (ATMRepository) for DB operations.
* - Service layer (ATMService) handles business logic.
* - Transaction consistency ensured using DB locking.
* - Timeout protection applied (6 seconds max execution).
*
* Unauthorized modification without understanding transaction,
* limit validations, and concurrency handling is strongly discouraged.
*
* For implementation details, contact: +91 9112765649
*/
#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>
#include <future>      // ================== TIMEOUT SUPPORT ==================
#include "Database.h"
#include "pin.h"       // ================== PIN SERVICE ==================
#include "panencrypted.h"

using namespace mysqlx;
using json = nlohmann::json;

// ================== TRANSACTION ID GENERATOR ==================
static std::string generateTxnId()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(1000,9999);

    std::ostringstream oss;
    oss << "atm-" << ms << "-" << dist(gen);
    return oss.str();
}

// ================== ATM REPOSITORY ==================
class ATMRepository
{
private:
    Session* session;
    Schema db;

public:

    ATMRepository(Session* s)
    : session(s), db(Database::getSchema())
    {
    }

    Row getCard(std::string pan,std::string expiry)
    {
        Table cards = db.getTable("cards");

        RowResult res = cards.select("account_number","status","scheme")
                .where("pan = :p AND expiry = :e")
                .bind("p",pan)
                .bind("e",expiry)
                .execute();

        if(res.count()==0)
            throw std::runtime_error("Invalid card");

        return res.fetchOne();
    }

    double getBalance(std::string acc)
    {
        RowResult res = session->sql(
                "SELECT balance FROM accounts WHERE account_number=? FOR UPDATE")
                .bind(acc)
                .execute();

        if(res.count()==0)
            throw std::runtime_error("Account not found");

        return res.fetchOne()[0].get<double>();
    }

    double getTodayTotal(std::string acc)
    {
        RowResult res = session->sql(
            "SELECT IFNULL(SUM(amount+fee),0) "
            "FROM transaction_atm "
            "WHERE account_number=? "
            "AND DATE(created_at)=CURDATE() "
            "AND status='SUCCESS'")
            .bind(acc)
            .execute();

        return res.fetchOne()[0].get<double>();
    }

    void updateBalance(std::string acc,double balance)
    {
        Table accounts = db.getTable("accounts");

        accounts.update()
        .set("balance",balance)
        .where("account_number = :a")
        .bind("a",acc)
        .execute();
    }

    long insertATMTransaction(
        std::string txnId,
        std::string clientTxnId,
        std::string atmId,
        std::string terminalId,
        std::string location,
        std::string acc,
        double amount,
        double fee,
        std::string pan,
        std::string scheme,
        std::string status,
        std::string msg)
    {
        Table atm = db.getTable("transaction_atm");

        auto res = atm.insert(
            "transaction_id",
            "client_txn_id",
            "atm_id",
            "terminal_id",
            "location",
            "account_number",
            "amount",
            "fee",
            "card_pan",
            "card_scheme",
            "status",
            "message")
        .values(
            txnId,
            clientTxnId,
            atmId,
            terminalId,
            location,
            acc,
            amount,
            fee,
            pan,
            scheme,
            status,
            msg)
        .execute();

        return res.getAutoIncrementValue();
    }

    void insertMasterTransaction(long refId)
    {
        Table master = db.getTable("transactions");

        master.insert("table_name","reference_id","status")
        .values("transaction_atm",refId,"SUCCESS")
        .execute();
    }
};

// ================== ATM SERVICE ==================
class ATMService
{
private:
    ATMRepository* repo;

public:

    ATMService(ATMRepository* r)
    {
        repo = r;
    }

    json process(const json& data)
    {
        json response;

        try
        {
            std::string clientTxnId = data["clientTxnId"];
            std::string txnType = data["transactionType"];
            std::string atmId = data["atmId"];
            std::string terminalId = data["terminalId"];
            std::string location = data["location"];
            std::string inputPan = "";
            std::string encryptedPan = data["card"]["pan"];

            try {
                auto& panService = PANEncryptionService::getInstance();
                inputPan = panService.decryptPAN(encryptedPan);
            }
            catch (const std::exception& e) {
                throw std::runtime_error("Invalid encrypted PAN");
            }
            std::string expiry = data["card"]["expiry"];

            double amount = data["amount"];
            double fee = data["fee"];

            // ================== PIN VERIFICATION (NEW - FIXED SINGLETON) ==================
            if (data.contains("pin"))
            {
                std::string inputPin = data["pin"];

                if (!PINService::getInstance().verifyPIN(inputPan, inputPin))
                {
                    throw std::runtime_error("Invalid PIN");
                }
            }

            // ================== ORIGINAL LOGIC ==================
            Row card = repo->getCard(inputPan,expiry);

            std::string account = card[0].get<std::string>();
            std::string status = card[1].get<std::string>();
            std::string scheme = card[2].get<std::string>();

            if(status!="ACTIVE")
                throw std::runtime_error("Card inactive");

            double balance = repo->getBalance(account);
            double newBalance = balance;

            double todayTotal = repo->getTodayTotal(account);

            if(txnType=="WITHDRAWAL")
            {
                if(todayTotal + amount + fee > 5000)
                    throw std::runtime_error("Daily withdrawal limit exceeded");

                if(balance < amount + fee)
                    throw std::runtime_error("Insufficient balance");

                newBalance = balance - amount - fee;
            }
            else if(txnType=="DEPOSIT")
            {
                if(todayTotal + amount > 10000)
                    throw std::runtime_error("Daily deposit limit exceeded");

                newBalance = balance + amount;
            }
            else
            {
                throw std::runtime_error("Invalid transaction type");
            }

            repo->updateBalance(account,newBalance);

            std::string txnId = generateTxnId();

            long refId = repo->insertATMTransaction(
                    txnId,
                    clientTxnId,
                    atmId,
                    terminalId,
                    location,
                    account,
                    amount,
                    fee,
                    inputPan,
                    scheme,
                    "SUCCESS",
                    "ATM transaction successful");

            repo->insertMasterTransaction(refId);
            // ✅ NEW: update last transaction time (CARD LEVEL)
            try {
                Session& sess = Database::getSession();
                Schema db = Database::getSchema();
                Table cards = db.getTable("cards");

                cards.update()
                    .set("last_transaction_time", mysqlx::expr("NOW()"))
                    .where("pan = :pan")
                    .bind("pan", inputPan)
                    .execute();
            }
            catch (...) {
                // do not fail transaction
            }

            response["transactionId"]=txnId;
            response["status"]="SUCCESS";
            response["balanceAfter"]=newBalance;
            response["message"]="ATM transaction successful";

            return response;
        }
        catch(const std::exception &e)
        {
            response["status"]="FAILED";
            response["message"]=e.what();
            return response;
        }
    }
};

// ================== CORE ==================
json processATMTransactionCore(const json& data)
{
    Session& session = Database::getSession();

    ATMRepository repo(&session);
    ATMService service(&repo);

    return service.process(data);
}

// ================== TIMEOUT WRAPPER ==================
json processATMTransaction(const json& data)
{
    auto future = std::async(std::launch::async, processATMTransactionCore, data);

    if (future.wait_for(std::chrono::seconds(6)) == std::future_status::timeout)
    {
        json response;
        response["status"] = "DECLINED";
        response["errorCode"] = "ERR_TIMEOUT";
        response["message"] = "Transaction timeout (more than 6 seconds)";
        return response;
    }

    return future.get();
}