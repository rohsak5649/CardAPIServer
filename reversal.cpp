/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * REVERSAL ENGINE — IMPLEMENTATION  v1.0
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * DESIGN
 *  ┌──────────────────────────────────────────────────────────────────────┐
 *  │  Channel handler (atm / pos / mobile …)                             │
 *  │    ↓  detects TIMEOUT or network loss after debit committed          │
 *  │  processReversal(data)                                               │
 *  │    ↓                                                                 │
 *  │  Step 1 — DB health check (SELECT 1)                                 │
 *  │    ├── DB OK  →  credit account  →  write transaction_reversal       │
 *  │    │                            →  write transactions (master)       │
 *  │    │                            →  update original row reversal_status│
 *  │    └── DB DOWN → insert reversal_drop_file (offline queue)           │
 *  └──────────────────────────────────────────────────────────────────────┘
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "reversal.h"
#include "Database.h"
#include "TransactionLogger.h"
#include "AccountLockManager.h"

#include <chrono>
#include <future>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include <mysqlx/xdevapi.h>

using namespace mysqlx;
using json = nlohmann::json;

// ── Constants ─────────────────────────────────────────────────────────────────
inline constexpr int REVERSAL_TIMEOUT_SECONDS = 10;   // async guard

// ── TxnId generator ───────────────────────────────────────────────────────────
[[nodiscard]] static std::string makeReversalId() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::mt19937 gen{std::random_device{}()};
    return "rev-" + std::to_string(ms) + "-" +
           std::to_string(std::uniform_int_distribution<>(1000, 9999)(gen));
}

// ═══════════════════════════════════════════════════════════════════════════════
//  REPOSITORY  (SRP: only DB access)
// ═══════════════════════════════════════════════════════════════════════════════
class ReversalRepository {
    Session* s_;
    Schema   db_;
public:
    explicit ReversalRepository(Session* s)
        : s_(s), db_(s->getSchema("bankingdb")) {}

    // ── DB health (SELECT 1) ─────────────────────────────────────────────────
    [[nodiscard]] bool isDbAlive() noexcept {
        TransactionLogger::ScopedFunctionTrace trace("ReversalRepository::isDbAlive");
        try {
            s_->sql("SELECT 1").execute();
            trace.success();
            return true;
        } catch (...) {
            trace.fail("DB health check failed — DB appears to be down");
            return false;
        }
    }

    // ── Explicit transaction control ──────────────────────────────────────────
    void beginTransaction() { s_->sql("START TRANSACTION").execute(); }
    void commit()           { s_->sql("COMMIT").execute(); }
    void rollback() noexcept { try { s_->sql("ROLLBACK").execute(); } catch (...) {} }

    // ── Account balance (with row-lock) ──────────────────────────────────────
    [[nodiscard]] double getBalance(std::string_view acc) {
        TransactionLogger::ScopedFunctionTrace trace(
            "ReversalRepository::getBalance",
            {{"accountNumber", std::string(acc)}});
        auto res = s_->sql(
            "SELECT balance FROM accounts WHERE account_number=? FOR UPDATE")
            .bind(std::string(acc)).execute();
        if (res.count() == 0) {
            trace.fail("account not found");
            throw std::runtime_error("Account not found: " + std::string(acc));
        }
        double bal = res.fetchOne()[0].get<double>();
        trace.success({{"balance", std::to_string(bal)}});
        return bal;
    }

    // ── Credit back the amount+fee ───────────────────────────────────────────
    void creditBack(std::string_view acc, double amount, double fee) {
        TransactionLogger::ScopedFunctionTrace trace(
            "ReversalRepository::creditBack",
            {{"accountNumber", std::string(acc)},
             {"amount",        std::to_string(amount)},
             {"fee",           std::to_string(fee)}});
        double current = getBalance(acc);
        double newBal  = current + amount + fee;
        db_.getTable("accounts").update()
            .set("balance", newBal)
            .where("account_number = :a")
            .bind("a", std::string(acc))
            .execute();
        trace.success({{"newBalance", std::to_string(newBal)}});
    }

    // ── Insert into transaction_reversal ─────────────────────────────────────
    [[nodiscard]] long insertReversalTxn(
        std::string_view reversalId,
        std::string_view clientTxnId,
        std::string_view originalTxnId,
        std::string_view originalTable,
        long             originalRefId,
        std::string_view channel,
        std::string_view acc,
        double           amount,
        double           fee,
        std::string_view cardPan,
        std::string_view cardScheme,
        std::string_view reason,
        std::string_view status,
        std::string_view message)
    {
        TransactionLogger::ScopedFunctionTrace trace(
            "ReversalRepository::insertReversalTxn",
            {{"reversalId",    std::string(reversalId)},
             {"originalTxnId", std::string(originalTxnId)},
             {"status",        std::string(status)}});

        auto res = db_.getTable("transaction_reversal").insert(
            "reversal_transaction_id",
            "client_txn_id",
            "original_transaction_id",
            "original_table_name",
            "original_reference_id",
            "channel",
            "account_number",
            "amount",
            "fee",
            "card_pan",
            "card_scheme",
            "reason",
            "status",
            "message")
            .values(
                std::string(reversalId),
                std::string(clientTxnId),
                std::string(originalTxnId),
                std::string(originalTable),
                originalRefId,
                std::string(channel),
                std::string(acc),
                amount,
                fee,
                std::string(cardPan),
                std::string(cardScheme),
                std::string(reason),
                std::string(status),
                std::string(message))
            .execute();

        long refId = res.getAutoIncrementValue();
        trace.success({{"referenceId", std::to_string(refId)}});
        return refId;
    }

    // ── Insert into master transactions table ────────────────────────────────
    void insertMasterTxn(long refId, std::string_view status) {
        TransactionLogger::ScopedFunctionTrace trace(
            "ReversalRepository::insertMasterTxn",
            {{"referenceId", std::to_string(refId)}});
        db_.getTable("transactions").insert(
            "table_name", "reference_id", "status")
            .values("transaction_reversal", refId, std::string(status))
            .execute();
        trace.success();
    }

    // ── Mark original channel row reversal_status = 'REVERSED' ──────────────
    // Works for any channel table that has a reversal_status column.
    // Uses raw SQL so we can parametrise the table name.
    void markOriginalReversed(std::string_view tableName, long originalRefId) {
        TransactionLogger::ScopedFunctionTrace trace(
            "ReversalRepository::markOriginalReversed",
            {{"tableName",     std::string(tableName)},
             {"originalRefId", std::to_string(originalRefId)}});
        try {
            std::string sql =
                "UPDATE `" + std::string(tableName) +
                "` SET reversal_status = 'REVERSED' WHERE id = ?";
            s_->sql(sql).bind(originalRefId).execute();
            trace.success();
        } catch (const std::exception& ex) {
            // Best-effort: the channel table might not have reversal_status.
            // Log and continue — the reversal record is still saved.
            trace.fail(
                "could not mark original row reversed (column may be missing)",
                {{"error", ex.what()}});
        }
    }

    // ── Write to reversal_drop_file when DB is down ──────────────────────────
    // This is the ONLY write that happens on a *separate* connection attempt.
    // If that also fails the caller returns a structured error.
    [[nodiscard]] bool writeDropFile(
        std::string_view reversalId,
        std::string_view originalTxnId,
        std::string_view originalTable,
        long             originalRefId,
        std::string_view channel,
        std::string_view acc,
        double           amount,
        double           fee,
        std::string_view reason,
        std::string_view rawPayload)
    {
        TransactionLogger::ScopedFunctionTrace trace(
            "ReversalRepository::writeDropFile",
            {{"reversalId", std::string(reversalId)}});
        try {
            db_.getTable("reversal_drop_file").insert(
                "reversal_id",
                "original_transaction_id",
                "original_table_name",
                "original_reference_id",
                "channel",
                "account_number",
                "amount",
                "fee",
                "reason",
                "raw_payload",
                "retry_status")
                .values(
                    std::string(reversalId),
                    std::string(originalTxnId),
                    std::string(originalTable),
                    originalRefId,
                    std::string(channel),
                    std::string(acc),
                    amount,
                    fee,
                    std::string(reason),
                    std::string(rawPayload),
                    "PENDING")
                .execute();
            trace.success();
            return true;
        } catch (const std::exception& ex) {
            trace.fail("writeDropFile failed", {{"error", ex.what()}});
            return false;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  CORE LOGIC
// ═══════════════════════════════════════════════════════════════════════════════
[[nodiscard]] static json processReversalCore(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace(
        "processReversalCore",
        {{"channel",             data.value("channel", "")},
         {"originalTxnId",       data.value("originalTransactionId", "")},
         {"accountNumber",       data.value("accountNumber", "")}});

    // ── Unpack required fields ────────────────────────────────────────────────
    std::string originalTxnId  = data.value("originalTransactionId", "");
    std::string originalTable  = data.value("originalTableName",     "");
    long        originalRefId  = data.value("originalReferenceId",   0LL);
    std::string acc            = data.value("accountNumber",         "");
    double      amount         = data.value("amount",                0.0);
    double      fee            = data.value("fee",                   0.0);
    std::string channel        = data.value("channel",               "UNKNOWN");
    std::string reason         = data.value("reason",                "TIMEOUT");
    std::string clientTxnId    = data.value("clientTxnId",           "");
    std::string cardPan        = data.value("cardPan",               "");
    std::string cardScheme     = data.value("cardScheme",            "");

    if (originalTxnId.empty() || originalTable.empty() ||
        acc.empty() || amount <= 0.0) {
        trace.fail("missing required reversal fields");
        return {
            {"status",    "FAILED"},
            {"errorCode", "ERR_REVERSAL_MISSING_FIELDS"},
            {"message",   "originalTransactionId, originalTableName, "
                          "accountNumber and amount are required"}
        };
    }

    std::string reversalId = makeReversalId();
    if (clientTxnId.empty()) clientTxnId = reversalId;

    // ── Step 1: Acquire DB connection ─────────────────────────────────────────
    try {
        Database::ScopedConnection sc;
        ReversalRepository repo(sc.operator->());

        // ── Step 2: DB health check ───────────────────────────────────────────
        if (!repo.isDbAlive()) {
            trace.checkpoint("db_down_detected",
                "DB unreachable — falling back to drop-file queue");

            // Try to write the drop file using the *same* connection attempt.
            // Even if the connection exists but operations fail, we capture it.
            bool saved = repo.writeDropFile(
                reversalId, originalTxnId, originalTable, originalRefId,
                channel, acc, amount, fee, reason, data.dump());

            if (saved) {
                trace.fail("DB down — reversal queued in drop file");
                return {
                    {"reversalId",           reversalId},
                    {"originalTransactionId", originalTxnId},
                    {"status",               "QUEUED"},
                    {"message",              "DB unavailable — reversal stored "
                                             "in drop-file queue for retry"},
                    {"dropFileQueued",       true}
                };
            } else {
                trace.fail("DB down AND drop-file write failed");
                return {
                    {"reversalId",           reversalId},
                    {"originalTransactionId", originalTxnId},
                    {"status",               "FAILED"},
                    {"errorCode",            "ERR_REVERSAL_DB_DOWN"},
                    {"message",              "DB is down and drop-file write "
                                             "also failed — manual intervention required"},
                    {"dropFileQueued",       false}
                };
            }
        }

        // ── Step 3: Credit back amount+fee under account lock ─────────────────
        {
            AccountLockManager::ScopedLock accLock(
                AccountLockManager::getInstance(), acc, TxnPriority::CREDIT);

            repo.beginTransaction();
            try {
                repo.creditBack(acc, amount, fee);

                // ── Step 4: Write transaction_reversal row ───────────────────
                long refId = repo.insertReversalTxn(
                    reversalId, clientTxnId,
                    originalTxnId, originalTable, originalRefId,
                    channel, acc, amount, fee,
                    cardPan, cardScheme, reason,
                    "REVERSED", "Reversal applied — " + reason);

                // ── Step 5: Write master transactions row ────────────────────
                repo.insertMasterTxn(refId, "REVERSED");

                // ── Step 6: Mark original row reversed (best-effort) ─────────
                repo.markOriginalReversed(originalTable, originalRefId);

                repo.commit();

                trace.success({
                    {"reversalId",  reversalId},
                    {"refId",       std::to_string(refId)},
                    {"channel",     channel},
                    {"amount",      std::to_string(amount)},
                    {"fee",         std::to_string(fee)}
                });

                return {
                    {"reversalId",            reversalId},
                    {"originalTransactionId", originalTxnId},
                    {"status",                "REVERSED"},
                    {"channel",               channel},
                    {"accountNumber",         acc},
                    {"amountCredited",        amount + fee},
                    {"message",               "Reversal successful — amount credited back"}
                };

            } catch (...) {
                repo.rollback();
                throw;
            }
        }

    } catch (const std::exception& ex) {
        // DB acquire failed (pool exhausted, down, etc.) — try drop file
        // on a fresh connection attempt:
        trace.checkpoint("db_acquire_exception",
            "DB acquire threw — attempting drop-file fallback",
            {{"error", ex.what()}});

        try {
            Database::ScopedConnection sc2;
            ReversalRepository repo2(sc2.operator->());
            bool saved = repo2.writeDropFile(
                reversalId, originalTxnId, originalTable, originalRefId,
                channel, acc, amount, fee, reason, data.dump());

            if (saved) {
                trace.fail("DB exception — reversal queued in drop file",
                           {{"error", ex.what()}});
                return {
                    {"reversalId",            reversalId},
                    {"originalTransactionId", originalTxnId},
                    {"status",                "QUEUED"},
                    {"message",               std::string("DB error — reversal stored in "
                                             "drop-file queue. Error: ") + ex.what()},
                    {"dropFileQueued",        true}
                };
            }
        } catch (...) {
            /* drop-file write also failed — fall through to hard error */
        }

        trace.fail("reversal failed — DB exception and drop-file unreachable",
                   {{"error", ex.what()}});
        return {
            {"reversalId",            reversalId},
            {"originalTransactionId", originalTxnId},
            {"status",                "FAILED"},
            {"errorCode",             "ERR_REVERSAL_DB"},
            {"message",               std::string("Reversal failed: ") + ex.what()}
        };
    }
}

// ── Public entry point ─────────────────────────────────────────────────────────
json processReversal(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace(
        "processReversal",
        {{"channel",       data.value("channel",               "")},
         {"originalTxnId", data.value("originalTransactionId", "")}});

    std::string uuid = data.value("_correlationUuid",
                                  TransactionLogger::currentUuid());
    TransactionLogger::ScopedContext scope(uuid, "REVERSAL");

    TransactionLogger::instance().logCurrent(
        "INFO", "reversal_scheduled",
        "Reversal request scheduled",
        {{"channel",             data.value("channel", "")},
         {"originalTxnId",       data.value("originalTransactionId", "")},
         {"accountNumber",       data.value("accountNumber", "")},
         {"amount",              std::to_string(data.value("amount", 0.0))},
         {"reason",              data.value("reason", "")}});

    auto future = std::async(std::launch::async, [data, uuid]() {
        TransactionLogger::ScopedContext asyncScope(uuid, "REVERSAL");
        TransactionLogger::instance().logCurrent(
            "INFO", "reversal_started", "Reversal processing started");
        return processReversalCore(data);
    });

    if (future.wait_for(std::chrono::seconds(REVERSAL_TIMEOUT_SECONDS)) ==
        std::future_status::timeout)
    {
        TransactionLogger::instance().logCurrent(
            "ERROR", "reversal_timeout",
            "Reversal processing timed out — possible data inconsistency",
            {{"timeoutSeconds", std::to_string(REVERSAL_TIMEOUT_SECONDS)},
             {"originalTxnId",  data.value("originalTransactionId", "")}});
        trace.fail("reversal async timeout");
        return {
            {"status",                "TIMEOUT"},
            {"errorCode",             "ERR_REVERSAL_TIMEOUT"},
            {"originalTransactionId", data.value("originalTransactionId","")},
            {"message",               "Reversal timed out — check reversal_drop_file "
                                      "table for pending retry"}
        };
    }

    json result = future.get();

    TransactionLogger::instance().logCurrent(
        result.value("status", "") == "REVERSED" ? "INFO" : "WARN",
        "reversal_finished", "Reversal processing finished",
        {{"reversalId",  result.value("reversalId",  "")},
         {"status",      result.value("status",      "")},
         {"errorCode",   result.value("errorCode",   "")}});

    if (result.value("status", "") == "REVERSED") {
        trace.success({{"reversalId", result.value("reversalId", "")}});
    } else {
        trace.fail("reversal did not complete normally",
                   {{"status",    result.value("status",    "")},
                    {"errorCode", result.value("errorCode", "")}});
    }
    return result;
}
