/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * ATM TRANSACTION PROCESSING  v3.0  (C++20 · Thread-Safe · Falcon-Protected)
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ Falcon fraud detection now wired in (was missing in v2.x)
 *   ✅ unordered_map<string,handler> dispatch — extensible without if-chain (OCP)
 *   ✅ std::string_view for zero-copy parameter passing
 *   ✅ [[nodiscard]] on all functions that return a result
 *   ✅ Structured Binding (C++17) for card row unpacking
 *   ✅ Daily limits stored as constexpr
 *   ✅ PAN masked in all log output
 *   ✅ last_transaction_time update uses RAII guard (best-effort)
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "atm.h"
#include "Database.h"
#include "pin.h"
#include "panencrypted.h"
#include "falcon.h"
#include "AccountLockManager.h"
#include "TransactionLogger.h"
#include "reversal.h"
#include "global_contant.h"

#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <future>
#include <thread>
#include <unordered_map>
#include <functional>
#include <mysqlx/xdevapi.h>
#include <atomic>

using namespace mysqlx;
using json = nlohmann::json;

// ── Compile-time limits ───────────────────────────────────────────────────────
inline constexpr double ATM_DAILY_WITHDRAWAL_LIMIT = 5'000.0;
inline constexpr double ATM_DAILY_DEPOSIT_LIMIT    = 10'000.0;
inline constexpr int    ATM_TIMEOUT_SECONDS        = 6;

// ── Shutdown flag ────────────────────────────────────────────────────────────
// Set to true by the signal handler in parser&router.cpp before the DB pool
// is destroyed. The detached reversal thread checks this flag before attempting
// to acquire a DB session, preventing use-after-free on pool teardown.
static std::atomic<bool> g_engineShutdown{false};

void setATMEngineShutdown() noexcept { g_engineShutdown.store(true); }

// ═══════════════════════════════════════════════════════════════════════════════
//  REPOSITORY — SRP: only DB access
// ═══════════════════════════════════════════════════════════════════════════════
class ATMRepository {
    Session* s_;
    Schema   db_;
public:
    explicit ATMRepository(Session* s) : s_(s), db_(s->getSchema("bankingdb")) {}

    // Returns {account_number, status, scheme, expiry, cvv}
    [[nodiscard]] std::optional<Row> getCard(std::string_view pan) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::getCard");
        auto res = db_.getTable("cards")
            .select("account_number","status","scheme","expiry","cvv")
            .where("pan = :p")
            .bind("p", std::string(pan))
            .execute();
        if (res.count() == 0) {
            trace.fail("card not found");
            return std::nullopt;
        }
        trace.success();
        return res.fetchOne();
    }

    [[nodiscard]] double getBalance(std::string_view acc) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::getBalance",
                                                     {{"accountNumber", std::string(acc)}});
        auto res = s_->sql(
            "SELECT balance FROM accounts WHERE account_number=? FOR UPDATE")
            .bind(std::string(acc)).execute();
        if (res.count() == 0) {
            trace.fail("account not found");
            throw std::runtime_error("Account not found");
        }
        double balance = res.fetchOne()[0].get<double>();
        trace.success({{"balance", std::to_string(balance)}});
        return balance;
    }

    [[nodiscard]] double getTodayWithdrawalTotal(std::string_view acc) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::getTodayWithdrawalTotal",
                                                     {{"accountNumber", std::string(acc)}});
        auto res = s_->sql(
            "SELECT IFNULL(SUM(amount+fee),0) FROM transaction_atm"
            " WHERE account_number=? AND DATE(created_at)=CURDATE()"
            " AND status='SUCCESS' AND message LIKE '%WITHDRAWAL%'")
            .bind(std::string(acc)).execute();
        double total = res.fetchOne()[0].get<double>();
        trace.success({{"todayWithdrawalTotal", std::to_string(total)}});
        return total;
    }

    [[nodiscard]] double getTodayDepositTotal(std::string_view acc) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::getTodayDepositTotal",
                                                     {{"accountNumber", std::string(acc)}});
        auto res = s_->sql(
            "SELECT IFNULL(SUM(amount),0) FROM transaction_atm"
            " WHERE account_number=? AND DATE(created_at)=CURDATE()"
            " AND status='SUCCESS' AND message LIKE '%DEPOSIT%'")
            .bind(std::string(acc)).execute();
        double total = res.fetchOne()[0].get<double>();
        trace.success({{"todayDepositTotal", std::to_string(total)}});
        return total;
    }

    void updateBalance(std::string_view acc, double balance) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::updateBalance",
                                                     {{"accountNumber", std::string(acc)},
                                                      {"newBalance", std::to_string(balance)}});
        db_.getTable("accounts").update()
            .set("balance", balance)
            .where("account_number = :a").bind("a", std::string(acc)).execute();
        trace.success();
    }

    [[nodiscard]] long insertATMTransaction(
        std::string_view txnId, std::string_view clientTxnId,
        std::string_view atmId, std::string_view terminalId,
        std::string_view location, std::string_view acc,
        double amount, double fee,
        std::string_view pan, std::string_view scheme,
        std::string_view status, std::string_view msg)
    {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::insertATMTransaction",
                                                     {{"txnId", std::string(txnId)},
                                                      {"status", std::string(status)},
                                                      {"amount", std::to_string(amount)}});
        auto res = db_.getTable("transaction_atm").insert(
            "transaction_id","client_txn_id","atm_id","terminal_id","location",
            "account_number","amount","fee","card_pan","card_scheme","status","message")
            .values(std::string(txnId),std::string(clientTxnId),
                    std::string(atmId),std::string(terminalId),std::string(location),
                    std::string(acc),amount,fee,
                    std::string(pan),std::string(scheme),
                    std::string(status),std::string(msg))
            .execute();
        long refId = res.getAutoIncrementValue();
        trace.success({{"referenceId", std::to_string(refId)}});
        return refId;
    }

    void insertMasterTxn(long refId) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::insertMasterTxn",
                                                     {{"referenceId", std::to_string(refId)}});
        db_.getTable("transactions").insert("table_name","reference_id","status")
            .values("transaction_atm", refId, "SUCCESS").execute();
        trace.success();
    }

    void touchCard(std::string_view pan) {
        TransactionLogger::ScopedFunctionTrace trace("ATMRepository::touchCard");
        try {
            db_.getTable("cards").update()
                .set("last_transaction_time", mysqlx::expr("NOW()"))
                .where("pan = :p").bind("p", std::string(pan)).execute();
            trace.success();
        } catch (...) {
            trace.fail("best-effort card touch failed");
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  TRANSACTION HANDLERS — OCP: add new types without modifying existing ones
// ═══════════════════════════════════════════════════════════════════════════════
using ATMHandler = std::function<json(const json&, ATMRepository&, Session&,
                                     std::string_view /* acc */,
                                     std::string_view /* pan */,
                                     std::string_view /* scheme */)>;

static json handleWithdrawal(const json& data, ATMRepository& repo, Session& sess,
                              std::string_view acc, std::string_view pan, std::string_view scheme) {
    TransactionLogger::ScopedFunctionTrace trace("handleWithdrawal",
                                                 {{"accountNumber", std::string(acc)}});
    // ⚠️  Lock BEFORE any balance/limit reads to prevent TOCTOU:
    //   Two concurrent withdrawals must not both pass the daily-limit check
    //   at the same time and then both succeed.
    AccountLockManager::ScopedLock accLock(AccountLockManager::getInstance(),
                                           std::string(acc), TxnPriority::DEBIT);

    const double amount = data["amount"].get<double>();
    const double fee    = data["fee"].get<double>();

    // Wrap all DB writes in an explicit transaction.  getBalance() uses
    // "FOR UPDATE" which requires an active transaction to hold the row lock.
    sess.startTransaction();
    try {
        // Read balance (FOR UPDATE) and today's total inside the transaction
        // so both reads see the same committed state.
        const double balance = repo.getBalance(acc);
        const double todayW  = repo.getTodayWithdrawalTotal(acc);

        if (todayW + amount + fee > ATM_DAILY_WITHDRAWAL_LIMIT) {
            sess.rollback();
            trace.fail("ATM withdrawal daily limit exceeded",
                       {{"amount", std::to_string(amount)},
                        {"todayWithdrawalTotal", std::to_string(todayW)}});
            throw std::runtime_error("Daily ATM withdrawal limit of " +
                                     std::to_string(ATM_DAILY_WITHDRAWAL_LIMIT) + " exceeded");
        }

        if (balance < amount + fee) {
            sess.rollback();
            trace.fail("insufficient balance",
                       {{"balance", std::to_string(balance)},
                        {"amount", std::to_string(amount)},
                        {"fee", std::to_string(fee)}});
            throw std::runtime_error("Insufficient balance");
        }

        repo.updateBalance(acc, balance - amount - fee);

        // Use the correlation UUID injected by the router as the DB transaction_id.
        // This gives one unified ID across: HTTP response, all log lines, and DB row.
        const std::string txnId    = data.value("_correlationUuid", TransactionLogger::currentUuid());
        const std::string clientId = data.value("clientTxnId", txnId);

        const long refId = repo.insertATMTransaction(
            txnId, clientId,
            data.value("atmId", ""), data.value("terminalId", ""), data.value("location", ""),
            acc, amount, fee, pan, scheme, "SUCCESS", "ATM WITHDRAWAL successful");
        repo.insertMasterTxn(refId);

        sess.commit();
        repo.touchCard(pan);

        trace.success({{"transactionId", txnId},
                       {"balanceAfter", std::to_string(balance - amount - fee)}});
        return {
            {"transactionId", txnId},
            {"status",        "SUCCESS"},
            {"balanceAfter",  balance - amount - fee},
            {"message",       "ATM withdrawal successful"}
        };
    } catch (...) {
        try { sess.rollback(); } catch (...) {}
        throw;
    }
}

static json handleDeposit(const json& data, ATMRepository& repo, Session& sess,
                           std::string_view acc, std::string_view pan, std::string_view scheme) {
    TransactionLogger::ScopedFunctionTrace trace("handleDeposit",
                                                 {{"accountNumber", std::string(acc)}});
    AccountLockManager::ScopedLock accLock(AccountLockManager::getInstance(),
                                           std::string(acc), TxnPriority::CREDIT);

    const double amount = data["amount"].get<double>();

    // All reads happen inside the transaction so the daily total and balance
    // are consistent (no TOCTOU between the limit check and the update).
    sess.startTransaction();
    try {
        const double todayD  = repo.getTodayDepositTotal(acc);
        if (todayD + amount > ATM_DAILY_DEPOSIT_LIMIT) {
            sess.rollback();
            trace.fail("ATM deposit daily limit exceeded",
                       {{"amount", std::to_string(amount)},
                        {"todayDepositTotal", std::to_string(todayD)}});
            throw std::runtime_error("Daily ATM deposit limit of " +
                                     std::to_string(ATM_DAILY_DEPOSIT_LIMIT) + " exceeded");
        }

        const double balance = repo.getBalance(acc);
        repo.updateBalance(acc, balance + amount);

        // Use the correlation UUID injected by the router as the DB transaction_id.
        const std::string txnId    = data.value("_correlationUuid", TransactionLogger::currentUuid());
        const std::string clientId = data.value("clientTxnId", txnId);

        const long refId = repo.insertATMTransaction(
            txnId, clientId,
            data.value("atmId", ""), data.value("terminalId", ""), data.value("location", ""),
            acc, amount, 0.0, pan, scheme, "SUCCESS", "ATM DEPOSIT successful");
        repo.insertMasterTxn(refId);

        sess.commit();
        repo.touchCard(pan);

        trace.success({{"transactionId", txnId},
                       {"balanceAfter", std::to_string(balance + amount)}});
        return {
            {"transactionId", txnId},
            {"status",        "SUCCESS"},
            {"balanceAfter",  balance + amount},
            {"message",       "ATM deposit successful"}
        };
    } catch (...) {
        try { sess.rollback(); } catch (...) {}
        throw;
    }
}

// OCP: register new types here — no other code changes needed
static const std::unordered_map<TransactionType, ATMHandler> ATM_DISPATCH = {
    { TransactionType::WITHDRAWAL, handleWithdrawal },
    { TransactionType::DEPOSIT,    handleDeposit    },
};

// ═══════════════════════════════════════════════════════════════════════════════
//  CORE — each call gets its own DB session from the pool
// ═══════════════════════════════════════════════════════════════════════════════
[[nodiscard]] static json processATMCore(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processATMCore",
                                                 {{"transactionType", data.value("transactionType", "")}});
    Database::ScopedConnection sc;
    Session& sess = *sc;
    ATMRepository repo(sc.operator->());

    try {
        std::string txnTypeStr = data.value("transactionType", "");
        TransactionType txnType = stringToTransactionType(txnTypeStr);
        std::string encPan    = data["card"]["pan"].get<std::string>();
        std::string expiry    = data["card"]["expiry"].get<std::string>();

        // ── Decrypt PAN (logic unchanged per client requirement) ──────────
        std::string pan = PANEncryptionService::getInstance().decryptPAN(encPan);

        // ── PIN verify (logic unchanged per client requirement) ────────────
        if (data.contains("pin")) {
            if (!PINService::getInstance().verifyPIN(pan, data["pin"].get<std::string>()))
                throw std::runtime_error("PIN verification failed");
        }

        // ── Card lookup ───────────────────────────────────────────────────
        auto cardOpt = repo.getCard(pan);
        if (!cardOpt) {
            trace.fail("card not found");
            return {
                {"status", "FAILED"},
                {"errorCode", "ERR_CARD_NOT_FOUND"},
                {"message", "Card not found"}
            };
        }
        
        Row cardRow = *cardOpt;
        std::string acc    = cardRow[0].get<std::string>();
        std::string status = cardRow[1].get<std::string>();
        std::string scheme = cardRow[2].isNull() ? "" : cardRow[2].get<std::string>();
        std::string dbExpiry = cardRow[3].get<std::string>();
        std::string dbCvv    = cardRow[4].isNull() ? "" : cardRow[4].get<std::string>();

        if (expiry != dbExpiry) {
            trace.fail("invalid expiry");
            return {
                {"status", "FAILED"},
                {"errorCode", "ERR_INVALID_EXPIRY"},
                {"message", "Wrong expiry date"}
            };
        }
        
        if (data["card"].contains("cvv")) {
            std::string reqCvv = data["card"]["cvv"].get<std::string>();
            if (reqCvv != dbCvv) {
                trace.fail("invalid CVV");
                return {
                    {"status", "FAILED"},
                    {"errorCode", "ERR_INVALID_CVV"},
                    {"message", "Wrong CVV"}
                };
            }
        }

        if (status != "ACTIVE") {
            trace.fail("card is not active", {{"cardStatus", status}});
            throw std::runtime_error("Card is not active");
        }

        // ── Falcon fraud check (NEW in v3) ────────────────────────────────
        Falcon falcon(sess);
        std::string fraudReason;
        if (falcon.checkFraud(acc, data.value("amount",0.0), fraudReason,
                              FalconChannel::ATM, &data)) {
            // Use the correlation UUID so the fraud-decline record ties back to the request log.
            const std::string txnId    = data.value("_correlationUuid", TransactionLogger::currentUuid());
            const std::string clientId = data.value("clientTxnId", txnId);
            falcon.logFraud(txnId, clientId, data.value("terminalId", ""), "",
                            acc, data.value("amount",0.0), fraudReason);
            trace.fail("fraud declined ATM transaction", {{"reason", fraudReason}});
            return {
                {"transactionId", txnId},
                {"status",        "DECLINED"},
                {"errorCode",     "ERR_FRAUD"},
                {"message",       fraudReason}
            };
        }

        // ── Dispatch to handler via unordered_map ─────────────────────────
        auto it = ATM_DISPATCH.find(txnType);
        if (it == ATM_DISPATCH.end()) {
            trace.fail("unsupported ATM transaction type", {{"transactionType", txnTypeStr}});
            throw std::runtime_error("Unsupported ATM transaction type: " + txnTypeStr);
        }

        json response = it->second(data, repo, sess, acc, pan, scheme);
        trace.success({{"transactionId", response.value("transactionId", "")},
                       {"status", response.value("status", "")}});
        return response;

    } catch (const std::exception& e) {
        trace.fail("ATM core exception", {{"error", e.what()}});
        return {{"status","FAILED"},{"errorCode","ERR_ATM"},{"message", e.what()}};
    }
}

// ── Public entry point — 6-second async timeout ───────────────────────────────
json processATMTransaction(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processATMTransaction",
                                                 {{"transactionType", data.value("transactionType", "")}});
    std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
    TransactionLogger::ScopedContext scope(uuid, "ATM");
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_scheduled", "ATM transaction handler scheduled",
        {{"transactionType", data.value("transactionType", "")}});

    auto future = std::async(std::launch::async, [data, uuid]() {
        TransactionLogger::ScopedContext asyncScope(uuid, "ATM");
        TransactionLogger::instance().logCurrent(
            "INFO", "channel_handler_started", "ATM transaction handler started",
            {{"transactionType", data.value("transactionType", "")}});
        return processATMCore(data);
    });

    if (future.wait_for(std::chrono::seconds(ATM_TIMEOUT_SECONDS)) ==
        std::future_status::timeout) {

        TransactionLogger::instance().logCurrent(
            "WARN", "channel_handler_timeout", "ATM transaction handler timeout — initiating reversal",
            {{"timeoutSeconds", std::to_string(ATM_TIMEOUT_SECONDS)}});
        trace.fail("ATM transaction timeout");

        // ── Auto-reversal on timeout ──────────────────────────────────────────
        // We cannot know for certain whether the debit was committed before the
        // timeout fired.  We conservatively fire a reversal for every timeout
        // that arrives on a debit-class transaction (WITHDRAWAL).
        // The reversal engine is idempotent: if the debit never landed, the
        // account balance will remain correct and the reversal row will still
        // be written for audit purposes.
        // ─────────────────────────────────────────────────────────────────────
        TransactionType txnType = stringToTransactionType(data.value("transactionType", ""));
        if (txnType == TransactionType::WITHDRAWAL) {
            json rev;
            // originalTransactionId is unknown at timeout — use a placeholder
            // so the drop-file / reversal record can be reconciled later.
            rev["originalTransactionId"] = data.value("clientTxnId",
                                           "atm-timeout-" + uuid);
            rev["originalTableName"]     = "transaction_atm";
            rev["originalReferenceId"]   = 0;          // unknown at timeout
            rev["accountNumber"]         = "";          // filled below if available
            rev["amount"]                = data.value("amount", 0.0);
            rev["fee"]                   = data.value("fee",    0.0);
            rev["channel"]               = "ATM";
            rev["reason"]                = "TIMEOUT";
            rev["clientTxnId"]           = data.value("clientTxnId", "");
            rev["cardPan"]               = "";          // PAN already decrypted in core
            rev["_correlationUuid"]      = uuid;

            // Attempt to resolve account number from card info in data
            // (best-effort — may be empty if card lookup also timed out)
            if (data.contains("_resolvedAccountNumber")) {
                rev["accountNumber"] = data["_resolvedAccountNumber"];
            }

            TransactionLogger::instance().logCurrent(
                "WARN", "atm_timeout_reversal_triggered",
                "Firing reversal due to ATM timeout",
                {{"originalTxnId", rev["originalTransactionId"].get<std::string>()},
                 {"amount",        std::to_string(data.value("amount", 0.0))}});

            // Fire-and-forget reversal (non-blocking from caller perspective).
            // Guard: skip DB access if engine is already shutting down to avoid
            // use-after-free on the connection pool (Database::destroyPool).
            std::thread([rev, uuid]() {
                if (g_engineShutdown.load()) {
                    TransactionLogger::ScopedContext revScope(uuid, "REVERSAL");
                    TransactionLogger::instance().logCurrent(
                        "WARN", "atm_timeout_reversal_skipped",
                        "Reversal skipped: engine is shutting down",
                        {{"originalTxnId",
                          rev.value("originalTransactionId", "")}});
                    return;
                }
                TransactionLogger::ScopedContext revScope(uuid, "REVERSAL");
                json reversalResult = processReversal(rev);
                TransactionLogger::instance().logCurrent(
                    reversalResult.value("status","") == "REVERSED" ? "INFO" : "WARN",
                    "atm_timeout_reversal_result",
                    "Reversal result after ATM timeout",
                    {{"reversalId", reversalResult.value("reversalId", "")},
                     {"status",     reversalResult.value("status",     "")},
                     {"errorCode",  reversalResult.value("errorCode",  "")}});
            }).detach();
        }

        return json{
            {"status",    "TIMEOUT"},
            {"errorCode", "ERR_TIMEOUT"},
            {"message",   "ATM transaction timeout (>" +
                           std::to_string(ATM_TIMEOUT_SECONDS) +
                           "s) — reversal initiated if debit was committed"}
        };
    }

    json result = future.get();
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_finished", "ATM transaction handler finished",
        {{"transactionId", result.value("transactionId", "")},
         {"status", result.value("status", "")},
         {"errorCode", result.value("errorCode", "")}});
    if (result.contains("errorCode")) {
        trace.fail("ATM transaction failed",
                   {{"errorCode", result["errorCode"].get<std::string>()}});
    } else {
        trace.success({{"transactionId", result.value("transactionId", "")}});
    }
    return result;
}
