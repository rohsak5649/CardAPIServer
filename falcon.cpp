/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * FALCON FRAUD DETECTION ENGINE — IMPLEMENTATION  v3.0
 * ─────────────────────────────────────────────────────────────────────
 *
 * FRAUD RULES APPLIED (in order):
 *   1. Same-second detection  — any channel active in last 1 second
 *   2. Per-channel velocity   — >5 txns in 60 sec on the same channel
 *   3. Cross-channel velocity — >5 txns combined across ALL channels in 60 sec
 *   4. Amount spike           — current amount > 3× 7-day average
 *
 * ALL 7 CHANNELS COVERED:
 *   ATM · MOBILE · POS · ECOM · QR · RINGPAY · (ISSUER exempt — no money out)
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "falcon.h"
#include "Database.h"
#include <iostream>
#include <sstream>

// ── Constructor ───────────────────────────────────────────────────────────────
Falcon::Falcon(Session& session) : sess_(session) {
    sess_.sql("USE bankingdb").execute();
}

// ── Generic row counter ───────────────────────────────────────────────────────
int Falcon::countRows_(const std::string& sql, const std::string& acc) {
    try {
        auto res = sess_.sql(sql).bind(acc).execute();
        int cnt = 0;
        while (res.fetchOne()) ++cnt;
        return cnt;
    } catch (...) { return 0; }
}

// ── Same-second: any txn on `table` for this account in last 1 second ─────────
bool Falcon::sameSecond_(std::string_view acc, std::string_view table) {
    std::string sql =
        "SELECT 1 FROM " + std::string(table) +
        " WHERE account_number = ?"
        " AND created_at >= NOW() - INTERVAL 1 SECOND LIMIT 1";
    try {
        auto res = sess_.sql(sql).bind(std::string(acc)).execute();
        return res.count() > 0;
    } catch (...) { return false; }
}

// ── Per-channel velocity ──────────────────────────────────────────────────────
bool Falcon::velocity_(std::string_view acc, std::string_view table) {
    std::string sql =
        "SELECT id FROM " + std::string(table) +
        " WHERE account_number = ?"
        " AND created_at >= NOW() - INTERVAL " +
        std::to_string(FALCON_VELOCITY_WINDOW) + " SECOND"
        " LIMIT " + std::to_string(FALCON_VELOCITY_LIMIT);
    return countRows_(sql, std::string(acc)) >= FALCON_VELOCITY_LIMIT;
}

// ── Cross-channel velocity (UNION across all 6 money-out channels) ────────────
bool Falcon::crossChannelVelocity_(std::string_view acc) {
    // We use a UNION to count total transactions across all channels in window
    std::string sql =
        "SELECT COUNT(*) FROM ("
        "  SELECT id FROM transaction_atm    WHERE account_number=? AND created_at >= NOW()-INTERVAL 60 SECOND"
        "  UNION ALL"
        "  SELECT id FROM transaction_mobile WHERE account_number=? AND created_at >= NOW()-INTERVAL 60 SECOND"
        "  UNION ALL"
        "  SELECT id FROM transaction_pos    WHERE account_number=? AND created_at >= NOW()-INTERVAL 60 SECOND"
        "  UNION ALL"
        "  SELECT id FROM transaction_ecom   WHERE account_number=? AND created_at >= NOW()-INTERVAL 60 SECOND"
        "  UNION ALL"
        "  SELECT id FROM transaction_qrcode WHERE account_number=? AND created_at >= NOW()-INTERVAL 60 SECOND"
        "  UNION ALL"
        "  SELECT id FROM transaction_ringpay WHERE account_number=? AND created_at >= NOW()-INTERVAL 60 SECOND"
        ") AS combined";

    try {
        std::string a = std::string(acc);
        auto res = sess_.sql(sql)
            .bind(a).bind(a).bind(a).bind(a).bind(a).bind(a)
            .execute();
        auto row = res.fetchOne();
        if (!row.isNull() && !row[0].isNull()) {
            int total = row[0].get<int>();
            return total >= FALCON_VELOCITY_LIMIT;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Falcon] crossChannelVelocity_ error: " << ex.what() << "\n";
    }
    return false;
}

// ── Amount spike: current > 3× 7-day average ─────────────────────────────────
bool Falcon::amountSpike_(std::string_view acc, double amount) {
    // Combine average from all channels
    std::string sql =
        "SELECT AVG(amt) FROM ("
        "  SELECT amount AS amt FROM transaction_atm    WHERE account_number=? AND created_at >= NOW()-INTERVAL 7 DAY AND status='SUCCESS'"
        "  UNION ALL"
        "  SELECT amount FROM transaction_mobile WHERE account_number=? AND created_at >= NOW()-INTERVAL 7 DAY AND status='SUCCESS'"
        "  UNION ALL"
        "  SELECT amount FROM transaction_pos    WHERE account_number=? AND created_at >= NOW()-INTERVAL 7 DAY AND status='SUCCESS'"
        "  UNION ALL"
        "  SELECT amount FROM transaction_ecom   WHERE account_number=? AND created_at >= NOW()-INTERVAL 7 DAY AND status='SUCCESS'"
        "  UNION ALL"
        "  SELECT amount FROM transaction_qrcode WHERE account_number=? AND created_at >= NOW()-INTERVAL 7 DAY AND status='SUCCESS'"
        "  UNION ALL"
        "  SELECT amount FROM transaction_ringpay WHERE account_number=? AND created_at >= NOW()-INTERVAL 7 DAY AND status='SUCCESS'"
        ") AS history";

    try {
        std::string a = std::string(acc);
        auto res = sess_.sql(sql)
            .bind(a).bind(a).bind(a).bind(a).bind(a).bind(a)
            .execute();
        auto row = res.fetchOne();
        if (!row.isNull() && !row[0].isNull()) {
            double avg = row[0].get<double>();
            if (avg > 0.0 && amount > FALCON_SPIKE_MULTIPLIER * avg)
                return true;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Falcon] amountSpike_ error: " << ex.what() << "\n";
    }
    return false;
}

// ── Main check ────────────────────────────────────────────────────────────────
bool Falcon::checkFraud(std::string_view     accountNumber,
                        double               amount,
                        std::string&         reason,
                        FalconChannel        channel) {

    // Channel-specific table name mapping
    struct TableEntry { FalconChannel ch; const char* table; };
    static constexpr TableEntry TABLES[] = {
        { FalconChannel::ATM,     "transaction_atm"     },
        { FalconChannel::MOBILE,  "transaction_mobile"  },
        { FalconChannel::POS,     "transaction_pos"     },
        { FalconChannel::ECOM,    "transaction_ecom"    },
        { FalconChannel::QRCODE,  "transaction_qrcode"  },
        { FalconChannel::RINGPAY, "transaction_ringpay" },
    };

    // ── Rule 1: Same-second on any channel ───────────────────────────────
    for (const auto& t : TABLES) {
        if (channel == FalconChannel::ALL || channel == t.ch) {
            if (sameSecond_(accountNumber, t.table)) {
                reason = "Duplicate transaction detected within 1 second";
                return true;
            }
        }
    }

    // ── Rule 2: Per-channel velocity ─────────────────────────────────────
    for (const auto& t : TABLES) {
        if (channel == FalconChannel::ALL || channel == t.ch) {
            if (velocity_(accountNumber, t.table)) {
                reason = "Velocity limit exceeded: >" +
                         std::to_string(FALCON_VELOCITY_LIMIT) +
                         " transactions in 60 seconds on " +
                         std::string(t.table);
                return true;
            }
        }
    }

    // ── Rule 3: Cross-channel velocity ───────────────────────────────────
    if (channel == FalconChannel::ALL) {
        if (crossChannelVelocity_(accountNumber)) {
            reason = "Cross-channel velocity limit exceeded (>" +
                     std::to_string(FALCON_VELOCITY_LIMIT) +
                     " txns across all channels in 60 sec)";
            return true;
        }
    }

    // ── Rule 4: Amount spike ─────────────────────────────────────────────
    if (amountSpike_(accountNumber, amount)) {
        reason = "Amount spike detected: " +
                 std::to_string(amount) +
                 " exceeds " +
                 std::to_string(FALCON_SPIKE_MULTIPLIER) +
                 "× 7-day average";
        return true;
    }

    return false;
}

// ── logFraud() ────────────────────────────────────────────────────────────────
void Falcon::logFraud(std::string_view txnId,
                      std::string_view clientTxnId,
                      std::string_view deviceId,
                      std::string_view mobileNo,
                      std::string_view accountNumber,
                      double           amount,
                      std::string_view reason) {
    try {
        auto ins = sess_.sql(
            "INSERT INTO transaction_falcon"
            " (transaction_id, client_txn_id, device_id, mobile_number,"
            "  account_number, amount, fraud_reason, status)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)")
            .bind(std::string(txnId))
            .bind(std::string(clientTxnId))
            .bind(std::string(deviceId))
            .bind(std::string(mobileNo))
            .bind(std::string(accountNumber))
            .bind(amount)
            .bind(std::string(reason))
            .bind("DECLINED")
            .execute();

        long id = ins.getAutoIncrementValue();

        sess_.sql(
            "INSERT INTO transactions (table_name, reference_id, status)"
            " VALUES (?, ?, ?)")
            .bind("transaction_falcon")
            .bind(id)
            .bind("DECLINED")
            .execute();

    } catch (const std::exception& ex) {
        std::cerr << "[Falcon] logFraud error: " << ex.what() << "\n";
    }
}