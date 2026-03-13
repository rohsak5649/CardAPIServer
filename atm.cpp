#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>
#include "Database.h"

using namespace mysqlx;
using json = nlohmann::json;

////////////////////////////////////////////////////////////
/// TRANSACTION ID GENERATOR
////////////////////////////////////////////////////////////

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

////////////////////////////////////////////////////////////
/// ATM REPOSITORY (DATABASE OPERATIONS)
////////////////////////////////////////////////////////////

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

    ////////////////////////////////////////////////////////////
    /// FETCH CARD DETAILS
    ////////////////////////////////////////////////////////////

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

    ////////////////////////////////////////////////////////////
    /// LOCK ACCOUNT AND FETCH BALANCE
    ////////////////////////////////////////////////////////////

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

    ////////////////////////////////////////////////////////////
    /// CALCULATE TODAY'S TOTAL TRANSACTION AMOUNT
    ////////////////////////////////////////////////////////////

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

    ////////////////////////////////////////////////////////////
    /// UPDATE ACCOUNT BALANCE
    ////////////////////////////////////////////////////////////

    void updateBalance(std::string acc,double balance)
    {
        Table accounts = db.getTable("accounts");

        accounts.update()
        .set("balance",balance)
        .where("account_number = :a")
        .bind("a",acc)
        .execute();
    }

    ////////////////////////////////////////////////////////////
    /// INSERT ATM TRANSACTION RECORD
    ////////////////////////////////////////////////////////////

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

    ////////////////////////////////////////////////////////////
    /// INSERT MASTER TRANSACTION ENTRY
    ////////////////////////////////////////////////////////////

    void insertMasterTransaction(long refId)
    {
        Table master = db.getTable("transactions");

        master.insert("table_name","reference_id","status")
        .values("transaction_atm",refId,"SUCCESS")
        .execute();
    }
};

////////////////////////////////////////////////////////////
/// ATM SERVICE (BUSINESS LOGIC)
////////////////////////////////////////////////////////////

class ATMService
{
private:
    ATMRepository* repo;

public:

    ATMService(ATMRepository* r)
    {
        repo = r;
    }

    ////////////////////////////////////////////////////////////
    /// PROCESS ATM TRANSACTION
    ////////////////////////////////////////////////////////////

    json process(const json& data)
    {
        json response;

        try
        {
            ////////////////////////////////////////////////////
            /// EXTRACT INPUT DATA
            ////////////////////////////////////////////////////

            std::string clientTxnId = data["clientTxnId"];
            std::string txnType = data["transactionType"];
            std::string atmId = data["atmId"];
            std::string terminalId = data["terminalId"];
            std::string location = data["location"];
            std::string pan = data["card"]["pan"];
            std::string expiry = data["card"]["expiry"];

            double amount = data["amount"];
            double fee = data["fee"];

            ////////////////////////////////////////////////////
            /// VALIDATE CARD
            ////////////////////////////////////////////////////

            Row card = repo->getCard(pan,expiry);

            std::string account = card[0].get<std::string>();
            std::string status = card[1].get<std::string>();
            std::string scheme = card[2].get<std::string>();

            if(status!="ACTIVE")
                throw std::runtime_error("Card inactive");

            ////////////////////////////////////////////////////
            /// FETCH BALANCE AND LOCK ACCOUNT
            ////////////////////////////////////////////////////

            double balance = repo->getBalance(account);
            double newBalance = balance;

            ////////////////////////////////////////////////////
            /// CHECK DAILY LIMIT
            ////////////////////////////////////////////////////

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

            ////////////////////////////////////////////////////
            /// UPDATE ACCOUNT BALANCE
            ////////////////////////////////////////////////////

            repo->updateBalance(account,newBalance);

            ////////////////////////////////////////////////////
            /// GENERATE TRANSACTION ID
            ////////////////////////////////////////////////////

            std::string txnId = generateTxnId();

            ////////////////////////////////////////////////////
            /// INSERT ATM TRANSACTION
            ////////////////////////////////////////////////////

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

            ////////////////////////////////////////////////////
            /// INSERT MASTER TRANSACTION
            ////////////////////////////////////////////////////

            repo->insertMasterTransaction(refId);

            ////////////////////////////////////////////////////
            /// SUCCESS RESPONSE
            ////////////////////////////////////////////////////

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

////////////////////////////////////////////////////////////
/// API ENTRY FUNCTION
////////////////////////////////////////////////////////////

json processATMTransaction(const json& data)
{
    Session& session = Database::getSession();

    ATMRepository repo(&session);
    ATMService service(&repo);

    return service.process(data);
}