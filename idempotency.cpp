#include "idempotency.h"
#include "TransactionLogger.h"
#include <iostream>

using namespace mysqlx;

std::optional<IdempotencyRecord> Idempotency::checkOrRegister(const std::string& key, const std::string& path) {
    Database::ScopedConnection conn;
    Session& sess = *conn;

    try {
        sess.sql("INSERT INTO idempotency_keys (idempotency_key, request_path, status) VALUES (?, ?, 'IN_PROGRESS')")
            .bind(key, path)
            .execute();
        return std::nullopt;
    } catch (const std::exception& err) {
        if (std::string(err.what()).find("Duplicate entry") != std::string::npos) {
            // It's a duplicate, we will fetch it below
        } else {
            TransactionLogger::instance().logCurrent("ERROR", "idempotency_insert_failed", "Failed to insert idempotency key", {{"error", err.what()}});
            return std::nullopt;
        }
    }

    // If we reach here, there was a duplicate entry. We must fetch it.
    try {
        RowResult result = sess.sql("SELECT status, response_code, response_body FROM idempotency_keys WHERE idempotency_key = ?")
                               .bind(key)
                               .execute();
        
        if (result.count() > 0) {
            Row row = result.fetchOne();
            std::string status = (std::string)row[0];
            
            if (status == "IN_PROGRESS") {
                throw std::runtime_error("ERR_IDEMPOTENCY_IN_PROGRESS");
            }
            
            IdempotencyRecord record;
            record.idempotency_key = key;
            record.request_path = path;
            record.status = status;
            record.response_code = row[1].isNull() ? 500 : (int)row[1];
            
            if (!row[2].isNull()) {
                record.response_body = json::parse((std::string)row[2]);
            }
            
            return record;
        }
    } catch (const std::runtime_error& e) {
        throw; // Re-throw the IN_PROGRESS error
    } catch (const std::exception& e) {
        TransactionLogger::instance().logCurrent("ERROR", "idempotency_fetch_failed", "Failed to fetch idempotency key", {{"error", e.what()}});
    }
    
    return std::nullopt;
}

void Idempotency::update(const std::string& key, int response_code, const json& response_body) {
    Database::ScopedConnection conn;
    Session& sess = *conn;
    
    try {
        std::string status = (response_code >= 200 && response_code < 300) ? "COMPLETED" : "FAILED";
        
        sess.sql("UPDATE idempotency_keys SET status = ?, response_code = ?, response_body = ? WHERE idempotency_key = ?")
            .bind(status, response_code, response_body.dump(), key)
            .execute();
            
    } catch (const std::exception& e) {
        TransactionLogger::instance().logCurrent("ERROR", "idempotency_update_failed", "Failed to update idempotency key", {{"error", e.what()}});
    }
}
