#include "idempotency.h"
#include "DatabaseQueries.h"
#include "TransactionLogger.h"
#include <iostream>

using namespace mysqlx;

std::optional<IdempotencyRecord> Idempotency::checkOrRegister(const std::string& key, const std::string& path) {
    Database::ScopedConnection conn;
    Session& sess = *conn;

    try {
        DatabaseQueries::insertIdempotencyRecord(sess, key, path);
        // INSERT succeeded — this is a new request, caller should process it.
        return std::nullopt;
    } catch (const std::exception& err) {
        if (std::string(err.what()).find("Duplicate entry") != std::string::npos) {
            // It's a duplicate — look up the existing record.
        } else {
            TransactionLogger::instance().logCurrent("ERROR", "idempotency_insert_failed",
                "Failed to insert idempotency key", {{"error", err.what()}});
            // Non-duplicate DB error: fail open — let the request through rather
            // than silently dropping it. Log it and return nullopt.
            return std::nullopt;
        }
    }

    try {
        auto rec = DatabaseQueries::getIdempotencyRecord(sess, key);

        if (!rec) {
            // Race condition: between our Duplicate error and this SELECT, the
            // original request either completed and was deleted, or there's a
            // serious inconsistency. Fail safe: allow the request to proceed.
            TransactionLogger::instance().logCurrent("WARN", "idempotency_row_vanished",
                "Idempotency row vanished after duplicate error — allowing request through",
                {{"key", key}});
            return std::nullopt;
        }

        std::string status = rec->status;

        if (status == "IN_PROGRESS") {
            // Another thread/request is currently processing this key.
            // Caller must return 409 Conflict to the client.
            throw std::runtime_error("ERR_IDEMPOTENCY_IN_PROGRESS");
        }

        IdempotencyRecord record;
        record.idempotency_key = key;
        record.request_path = path;
        record.status = status;
        // response_code is 0 if NULL (not yet stored — shouldn't happen for
        // COMPLETED/FAILED, but be defensive). 500 would be misleading here.
        record.response_code = rec->responseCode;

        if (!rec->responseBody.empty()) {
            record.response_body = json::parse(rec->responseBody);
        }

        return record;
    } catch (const std::runtime_error& e) {
        throw; // Re-throw ERR_IDEMPOTENCY_IN_PROGRESS so caller returns 409
    } catch (const std::exception& e) {
        TransactionLogger::instance().logCurrent("ERROR", "idempotency_fetch_failed",
            "Failed to fetch idempotency key", {{"error", e.what()}});
    }

    return std::nullopt;
}

void Idempotency::update(const std::string& key, int response_code, const json& response_body) {
    Database::ScopedConnection conn;
    Session& sess = *conn;

    try {
        std::string status = (response_code >= 200 && response_code < 300) ? "COMPLETED" : "FAILED";

        DatabaseQueries::updateIdempotencyRecord(sess, key, status, response_code, response_body.dump());

    } catch (const std::exception& e) {
        TransactionLogger::instance().logCurrent("ERROR", "idempotency_update_failed",
            "Failed to update idempotency key", {{"error", e.what()}});
    }
}
