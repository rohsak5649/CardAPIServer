/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* FALCON FRAUD DETECTION ENGINE:
*
* OVERVIEW:
* Falcon is a real-time fraud detection module integrated across
* multiple transaction channels:
*    → MOBILE
*    → ECOM
*    → POS
*
* PURPOSE:
* - Detect suspicious transaction patterns.
* - Prevent fraud in high-frequency or abnormal activity scenarios.
* - Provide centralized fraud monitoring across all channels.
*
* 1. INPUT:
*    - accountNumber
*    - transaction amount
*    - transaction metadata (txnId, clientTxnId, etc.)
*
* 2. FRAUD CHECK MODULES:
*
*    A. SAME-SECOND DETECTION:
*    - Detects multiple transactions within 1 second.
*    - Applied across:
*        → transaction_mobile
*        → transaction_ecom
*        → transaction_pos
*    - Prevents bot attacks / rapid swipe fraud.
*
*    B. VELOCITY CHECK:
*    - Detects excessive transactions within 60 seconds.
*    - Threshold:
*        → 5 transactions per minute
*    - Applied across all channels.
*
* 3. CROSS-CHANNEL MONITORING:
*    - Fraud detection is NOT limited to a single channel.
*    - Combines activity from:
*        → Mobile + ECOM + POS
*    - Ensures holistic fraud detection.
*
* 4. DECISION ENGINE:
*    - If any fraud rule triggers:
*        → Transaction is DECLINED
*        → Reason is recorded
*
* 5. FRAUD LOGGING:
*    - Fraudulent transactions inserted into:
*        → transaction_falcon table
*    - Also logged into master transactions table.
*
* 6. RESPONSE:
*    - TRUE  → Fraud detected
*    - FALSE → Transaction safe
*
* SECURITY NOTES:
* - Works in real-time before transaction commit.
* - Prevents financial loss due to abnormal activity.
*
* DESIGN NOTES:
* - Modular structure for easy rule extension.
* - Can integrate:
*        → Device fingerprinting
*        → Geo-location anomaly detection
*        → ML-based scoring (future scope)
*
* CURRENT RULES:
* - Same-second detection
* - Velocity detection (5 txns / 60 sec)
*
* FUTURE ENHANCEMENTS:
* - Risk scoring engine
* - AI/ML fraud detection
* - Merchant-level fraud profiling
*
* Unauthorized modification without understanding fraud rules
* and transaction flow may compromise system security.
*
* For implementation details, contact: +91 9112765649
*/
#include "falcon.h"
#include "Database.h"

Falcon::Falcon(Session& session) : sess(session)
{
    sess.sql("USE bankingdb").execute();
}

// ================= MAIN FRAUD CHECK =================
bool Falcon::checkFraud(const std::string& accountNumber,
                        double amount,
                        std::string& reason)
{
    // 🔥 SAME SECOND (ALL CHANNELS)
    if (checkSameSecond(accountNumber) ||
        checkSameSecondForEcom(accountNumber) ||
        checkSameSecondForPOS(accountNumber)) {

        reason = "Multiple transactions within same second";
        return true;
    }

    // 🔥 VELOCITY (ALL CHANNELS)
    if (checkVelocity(accountNumber) ||
        checkVelocityForEcom(accountNumber) ||
        checkVelocityForPOS(accountNumber)) {

        reason = "More than 5 transactions within 60 seconds";
        return true;
    }

    return false;
}

// ================= MOBILE =================
bool Falcon::checkSameSecond(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT 1 FROM transaction_mobile "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 1 SECOND LIMIT 1"
    )
    .bind(accountNumber)
    .execute();

    return res.count() > 0;
}

bool Falcon::checkVelocity(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT id FROM transaction_mobile "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 60 SECOND LIMIT 5"
    )
    .bind(accountNumber)
    .execute();

    int count = 0;
    while (res.fetchOne()) count++;

    return count >= 5;
}

// ================= ECOM =================
bool Falcon::checkSameSecondForEcom(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT 1 FROM transaction_ecom "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 1 SECOND LIMIT 1"
    )
    .bind(accountNumber)
    .execute();

    return res.count() > 0;
}

bool Falcon::checkVelocityForEcom(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT id FROM transaction_ecom "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 60 SECOND LIMIT 5"
    )
    .bind(accountNumber)
    .execute();

    int count = 0;
    while (res.fetchOne()) count++;

    return count >= 5;
}

// ================= POS (🔥 NEW) =================
bool Falcon::checkSameSecondForPOS(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT 1 FROM transaction_pos "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 1 SECOND LIMIT 1"
    )
    .bind(accountNumber)
    .execute();

    return res.count() > 0;
}

bool Falcon::checkVelocityForPOS(const std::string& accountNumber)
{
    RowResult res = sess.sql(
        "SELECT id FROM transaction_pos "
        "WHERE account_number = ? "
        "AND created_at >= NOW() - INTERVAL 60 SECOND LIMIT 5"
    )
    .bind(accountNumber)
    .execute();

    int count = 0;
    while (res.fetchOne()) count++;

    return count >= 5;
}

// ================= FRAUD LOG =================
void Falcon::logFraud(const std::string& txnId,
                      const std::string& clientTxnId,
                      const std::string& deviceId,
                      const std::string& mobileNo,
                      const std::string& accountNumber,
                      double amount,
                      const std::string& reason)
{
    auto result = sess.sql(
        "INSERT INTO transaction_falcon "
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

    long id = result.getAutoIncrementValue();

    sess.sql(
        "INSERT INTO transactions (table_name, reference_id, status) "
        "VALUES (?, ?, ?)"
    )
    .bind("transaction_falcon")
    .bind(id)
    .bind("DECLINED")
    .execute();
}