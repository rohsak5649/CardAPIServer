#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>
#include <future>      // ================== TIMEOUT SUPPORT ==================
#include "Database.h"
#include "pin.h"       // ================== PIN SERVICE ==================

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
            std::string pan = data["card"]["pan"];
            std::string expiry = data["card"]["expiry"];

            double amount = data["amount"];
            double fee = data["fee"];

            // ================== PIN VERIFICATION (NEW - FIXED SINGLETON) ==================
            if (data.contains("pin"))
            {
                std::string inputPin = data["pin"];

                if (!PINService::getInstance().verifyPIN(pan, inputPin))
                {
                    throw std::runtime_error("Invalid PIN");
                }
            }

            // ================== ORIGINAL LOGIC ==================
            Row card = repo->getCard(pan,expiry);

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
                    pan,
                    scheme,
                    "SUCCESS",
                    "ATM transaction successful");

            repo->insertMasterTransaction(refId);

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