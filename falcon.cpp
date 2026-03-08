/*
* Copyright (c) Rohan Sakhare
 * All rights reserved.
 *
 * Falcon Fraud Detection Flow:
 * 1. Transaction request received from mobile channel.
 * 2. System checks rapid repeat transactions (same-second rule).
 * 3. System checks transaction velocity (max 5 per 60 seconds).
 * 4. If fraud detected → transaction declined.
 * 5. Fraud details logged in Falcon table and Master table.
 * 6. Safe transactions proceed to normal processing.
 *
 * Unauthorized copying or modification without understanding
 * the fraud control logic is discouraged.
 *
 * For implementation details, contact: +91 9112765649
 */
#include "falcon.h"

Falcon::Falcon(Session& session) : sess(session)
{
    sess.sql("USE bankingdb").execute();
}

bool Falcon::checkFraud(const std::string& accountNumber,
                        double amount,
                        std::string& reason)
{
    if (checkSameSecond(accountNumber)) {
        reason = "Multiple transactions within same second";
        return true;
    }

    if (checkVelocity(accountNumber)) {
        reason = "More than 5 transactions within 60 seconds";
        return true;
    }

    return false;
}
bool Falcon::checkSameSecond(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT 1 FROM bankingdb.transaction_mobile "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 1 SECOND "
        "LIMIT 1"
    )
    .bind(accountNumber)
    .execute();

    return res.count() > 0;
}
bool Falcon::checkVelocity(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT id FROM bankingdb.transaction_mobile "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 60 SECOND "
        "LIMIT 5"
    )
    .bind(accountNumber)
    .execute();

    int rowCount = 0;

    while (res.fetchOne()) {
        rowCount++;
    }

    return rowCount >= 5;
}
void Falcon::logFraud(const std::string& txnId,
                      const std::string& clientTxnId,
                      const std::string& deviceId,
                      const std::string& mobileNo,
                      const std::string& accountNumber,
                      double amount,
                      const std::string& reason)
{
    sess.sql("USE bankingdb").execute();

    auto result = sess.sql(
        "INSERT INTO bankingdb.transaction_falcon "
        "(transaction_id, client_txn_id, device_id, mobile_number, "
        "account_number, amount, fraud_reason, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
    )
    .bind(txnId)
    .bind(clientTxnId)
    .bind(deviceId)
    .bind(mobileNo)
    .bind(accountNumber)
    .bind(amount)
    .bind(reason)
    .bind("DECLINED")
    .execute();

    long childId = result.getAutoIncrementValue();

    sess.sql(
        "INSERT INTO bankingdb.transactions "
        "(table_name, reference_id, status) "
        "VALUES (?, ?, ?)"
    )
    .bind("transaction_falcon")
    .bind(childId)
    .bind("DECLINED")
    .execute();
}

