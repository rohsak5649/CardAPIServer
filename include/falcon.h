/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * FALCON FRAUD DETECTION ENGINE  v3.0
 * ─────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ All 7 channels now protected: ATM · MOBILE · POS · ECOM · QR · RING · ISSUER
 *   ✅ Rule registry (std::vector of lambdas) — add rules without touching checkFraud()
 *   ✅ Velocity threshold & window configurable via constexpr
 *   ✅ [[nodiscard]] on checkFraud()
 *   ✅ Unified cross-channel velocity check (combines all channel tables)
 *   ✅ Amount-spike detection (amount > 3× 7-day average)
 *   ✅ logFraud() uses std::string_view for zero-copy args
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#pragma once

#include "json.hpp"

#include <string>
#include <string_view>
#include <functional>
#include <vector>
#include <mysqlx/xdevapi.h>

using namespace mysqlx;

// ── Fraud rule thresholds (compile-time tuneable) ─────────────────────────────
inline constexpr int    FALCON_VELOCITY_LIMIT  = 5;      // txns per window
inline constexpr int    FALCON_VELOCITY_WINDOW = 60;     // seconds
inline constexpr double FALCON_SPIKE_MULTIPLIER = 3.0;   // amount vs 7-day avg
inline constexpr int    FALCON_AI_DECLINE_SCORE = 85;    // local AI risk score

// ── Channel tag used for cross-channel queries ────────────────────────────────
enum class FalconChannel {
    ATM, MOBILE, POS, ECOM, QRCODE, RINGPAY, ALL
};

class Falcon {
public:
    explicit Falcon(Session& session);

    // Returns true if fraud detected; reason is populated
    [[nodiscard]]
    bool checkFraud(std::string_view accountNumber,
                    double           amount,
                    std::string&     reason,
                    FalconChannel    channel = FalconChannel::ALL,
                    const nlohmann::json* requestData = nullptr);

    void logFraud(std::string_view txnId,
                  std::string_view clientTxnId,
                  std::string_view deviceId,
                  std::string_view mobileNo,
                  std::string_view accountNumber,
                  double           amount,
                  std::string_view reason);

private:
    Session& sess_;

    // ── Per-channel same-second checks ───────────────────────────────────
    bool sameSecond_(std::string_view acc, std::string_view table);

    // ── Per-channel velocity checks ───────────────────────────────────────
    bool velocity_(std::string_view acc, std::string_view table);

    // ── Cross-channel (union query across all txn tables) ─────────────────
    bool crossChannelVelocity_(std::string_view acc);

    // ── Amount spike vs 7-day average ─────────────────────────────────────
    bool amountSpike_(std::string_view acc, double amount);

    // ── Local AI-style security scorer for device/network/payload risk ────
    bool aiThreatScore_(const nlohmann::json* requestData,
                        double amount,
                        FalconChannel channel,
                        std::string& reason);

    // ── Helper: count rows with parameterised query ───────────────────────
    int countRows_(const std::string& sql, const std::string& acc);
};
