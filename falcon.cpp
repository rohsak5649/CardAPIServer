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
#include "TransactionLogger.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* channelName(FalconChannel channel) {
    switch (channel) {
        case FalconChannel::ATM: return "ATM";
        case FalconChannel::MOBILE: return "MOBILE";
        case FalconChannel::POS: return "POS";
        case FalconChannel::ECOM: return "ECOM";
        case FalconChannel::QRCODE: return "QRCODE";
        case FalconChannel::RINGPAY: return "RINGPAY";
        case FalconChannel::ALL: return "ALL";
    }
    return "UNKNOWN";
}

std::string lowerAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

std::string joinSignals(const std::vector<std::string>& signals) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < signals.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << signals[i];
    }
    return oss.str();
}

void addRisk(int& score,
             std::vector<std::string>& signals,
             int points,
             std::string signal) {
    score = std::min(100, score + points);
    if (std::find(signals.begin(), signals.end(), signal) == signals.end()) {
        signals.push_back(std::move(signal));
    }
}

const nlohmann::json* findObject(const nlohmann::json& root,
                                 std::string_view key) {
    if (!root.is_object()) {
        return nullptr;
    }
    auto it = root.find(std::string(key));
    if (it != root.end() && it->is_object()) {
        return &(*it);
    }
    return nullptr;
}

std::string stringValue(const nlohmann::json& root, std::string_view key) {
    if (!root.is_object()) {
        return "";
    }
    auto it = root.find(std::string(key));
    if (it == root.end() || it->is_null()) {
        return "";
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_boolean()) {
        return it->get<bool>() ? "true" : "false";
    }
    if (it->is_number()) {
        return it->dump();
    }
    return "";
}

std::string nestedStringValue(const nlohmann::json& root,
                              std::string_view objectKey,
                              std::string_view key) {
    const nlohmann::json* object = findObject(root, objectKey);
    return object == nullptr ? "" : stringValue(*object, key);
}

bool boolValue(const nlohmann::json& root, std::string_view key, bool& out) {
    if (!root.is_object()) {
        return false;
    }
    auto it = root.find(std::string(key));
    if (it == root.end() || it->is_null()) {
        return false;
    }
    if (it->is_boolean()) {
        out = it->get<bool>();
        return true;
    }
    if (it->is_number_integer()) {
        out = it->get<int>() != 0;
        return true;
    }
    if (it->is_string()) {
        const std::string value = lowerAscii(it->get<std::string>());
        if (value == "true" || value == "yes" || value == "1" ||
            value == "detected" || value == "fail" || value == "failed") {
            out = true;
            return true;
        }
        if (value == "false" || value == "no" || value == "0" ||
            value == "clean" || value == "pass" || value == "passed") {
            out = false;
            return true;
        }
    }
    return false;
}

bool nestedBoolValue(const nlohmann::json& root,
                     std::string_view objectKey,
                     std::string_view key,
                     bool& out) {
    const nlohmann::json* object = findObject(root, objectKey);
    return object != nullptr && boolValue(*object, key, out);
}

bool numberValue(const nlohmann::json& root,
                 std::string_view key,
                 double& out) {
    if (!root.is_object()) {
        return false;
    }
    auto it = root.find(std::string(key));
    if (it == root.end() || it->is_null()) {
        return false;
    }
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stod(it->get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool nestedNumberValue(const nlohmann::json& root,
                       std::string_view objectKey,
                       std::string_view key,
                       double& out) {
    const nlohmann::json* object = findObject(root, objectKey);
    return object != nullptr && numberValue(*object, key, out);
}

bool containsAny(std::string_view text,
                 const std::vector<std::string_view>& markers) {
    const std::string lowered = lowerAscii(text);
    for (std::string_view marker : markers) {
        if (lowered.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void scanJsonForAttackMarkers(const nlohmann::json& value,
                              int depth,
                              std::size_t& remainingChars,
                              bool& matched) {
    if (matched || depth > 4 || remainingChars == 0) {
        return;
    }

    static const std::vector<std::string_view> kPayloadMarkers{
        "<script", "javascript:", "onerror=", "union select", " or 1=1",
        "drop table", "../", "..\\", "/etc/passwd", "cmd.exe", "powershell",
        "wget ", "curl ", "${jndi:", "base64,", "xp_cmdshell", "sleep("
    };

    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (text.size() > remainingChars) {
            text.resize(remainingChars);
        }
        remainingChars -= text.size();
        matched = containsAny(text, kPayloadMarkers);
        return;
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            scanJsonForAttackMarkers(item, depth + 1, remainingChars, matched);
            if (matched || remainingChars == 0) {
                return;
            }
        }
        return;
    }

    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!it.key().empty() && it.key()[0] == '_') {
                continue;
            }
            if (containsAny(it.key(), kPayloadMarkers)) {
                matched = true;
                return;
            }
            scanJsonForAttackMarkers(it.value(), depth + 1, remainingChars, matched);
            if (matched || remainingChars == 0) {
                return;
            }
        }
    }
}

} // namespace

// ── Constructor ───────────────────────────────────────────────────────────────
Falcon::Falcon(Session& session) : sess_(session) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::Falcon");
    sess_.sql("USE bankingdb").execute();
    trace.success();
}

// ── Generic row counter ───────────────────────────────────────────────────────
int Falcon::countRows_(const std::string& sql, const std::string& acc) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::countRows_");
    try {
        auto res = sess_.sql(sql).bind(acc).execute();
        int cnt = 0;
        while (res.fetchOne()) ++cnt;
        trace.success({{"count", std::to_string(cnt)}});
        return cnt;
    } catch (...) {
        trace.fail("failed to count fraud rows");
        return 0;
    }
}

// ── Same-second: any txn on `table` for this account in last 1 second ─────────
bool Falcon::sameSecond_(std::string_view acc, std::string_view table) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::sameSecond_",
                                                 {{"table", std::string(table)}});
    std::string sql =
        "SELECT 1 FROM " + std::string(table) +
        " WHERE account_number = ?"
        " AND created_at >= NOW() - INTERVAL 1 SECOND LIMIT 1";
    try {
        auto res = sess_.sql(sql).bind(std::string(acc)).execute();
        bool matched = res.count() > 0;
        trace.success({{"matched", matched ? "true" : "false"}});
        return matched;
    } catch (...) {
        trace.fail("same-second fraud query failed");
        return false;
    }
}

// ── Per-channel velocity ──────────────────────────────────────────────────────
bool Falcon::velocity_(std::string_view acc, std::string_view table) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::velocity_",
                                                 {{"table", std::string(table)}});
    std::string sql =
        "SELECT id FROM " + std::string(table) +
        " WHERE account_number = ?"
        " AND created_at >= NOW() - INTERVAL " +
        std::to_string(FALCON_VELOCITY_WINDOW) + " SECOND"
        " LIMIT " + std::to_string(FALCON_VELOCITY_LIMIT);
    bool matched = countRows_(sql, std::string(acc)) >= FALCON_VELOCITY_LIMIT;
    trace.success({{"matched", matched ? "true" : "false"}});
    return matched;
}

// ── Cross-channel velocity (UNION across all 6 money-out channels) ────────────
bool Falcon::crossChannelVelocity_(std::string_view acc) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::crossChannelVelocity_");
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
            bool matched = total >= FALCON_VELOCITY_LIMIT;
            trace.success({{"total", std::to_string(total)},
                           {"matched", matched ? "true" : "false"}});
            return matched;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Falcon] crossChannelVelocity_ error: " << ex.what() << "\n";
        trace.fail("cross-channel velocity query failed", {{"error", ex.what()}});
    }
    trace.success({{"matched", "false"}});
    return false;
}

// ── Amount spike: current > 3× 7-day average ─────────────────────────────────
bool Falcon::amountSpike_(std::string_view acc, double amount) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::amountSpike_",
                                                 {{"amount", std::to_string(amount)}});
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
            if (avg > 0.0 && amount > FALCON_SPIKE_MULTIPLIER * avg) {
                trace.success({{"average", std::to_string(avg)}, {"matched", "true"}});
                return true;
            }
            trace.success({{"average", std::to_string(avg)}, {"matched", "false"}});
            return false;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Falcon] amountSpike_ error: " << ex.what() << "\n";
        trace.fail("amount spike query failed", {{"error", ex.what()}});
    }
    trace.success({{"matched", "false"}});
    return false;
}

// ── Local AI-style risk scoring for device/network/payload threat signals ────
bool Falcon::aiThreatScore_(const nlohmann::json* requestData,
                            double amount,
                            FalconChannel channel,
                            std::string& reason) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::aiThreatScore_",
                                                 {{"fraudChannel", channelName(channel)}});
    if (requestData == nullptr || !requestData->is_object()) {
        trace.success({{"riskScore", "0"}, {"decision", "NO_CONTEXT"}});
        return false;
    }

    const nlohmann::json& data = *requestData;
    int score = 0;
    std::vector<std::string> signals;

    auto addBoolRisk = [&](std::string_view key, int points, std::string signal) {
        bool value = false;
        if ((boolValue(data, key, value) ||
             nestedBoolValue(data, "security", key, value) ||
             nestedBoolValue(data, "device", key, value) ||
             nestedBoolValue(data, "_securityContext", key, value)) && value) {
            addRisk(score, signals, points, std::move(signal));
        }
    };

    auto addFalseRisk = [&](std::string_view key, int points, std::string signal) {
        bool value = true;
        if ((boolValue(data, key, value) ||
             nestedBoolValue(data, "security", key, value) ||
             nestedBoolValue(data, "device", key, value) ||
             nestedBoolValue(data, "_securityContext", key, value)) && !value) {
            addRisk(score, signals, points, std::move(signal));
        }
    };

    addBoolRisk("malwareDetected", 95, "malware_detected");
    addBoolRisk("malware", 95, "malware_detected");
    addBoolRisk("tamperDetected", 85, "app_tamper_detected");
    addBoolRisk("hookDetected", 85, "runtime_hook_detected");
    addBoolRisk("instrumentationDetected", 85, "instrumentation_detected");
    addBoolRisk("debuggerDetected", 75, "debugger_detected");
    addBoolRisk("rooted", 45, "rooted_device");
    addBoolRisk("jailbroken", 45, "jailbroken_device");
    addBoolRisk("emulator", 35, "emulator_device");
    addBoolRisk("proxyDetected", 25, "proxy_detected");
    addBoolRisk("vpnDetected", 25, "vpn_detected");
    addFalseRisk("appSignatureValid", 90, "invalid_app_signature");
    addFalseRisk("deviceBindingValid", 80, "device_binding_failed");

    std::string integrity = lowerAscii(stringValue(data, "deviceIntegrity"));
    if (integrity.empty()) integrity = lowerAscii(nestedStringValue(data, "security", "deviceIntegrity"));
    if (integrity.empty()) integrity = lowerAscii(nestedStringValue(data, "device", "deviceIntegrity"));
    if (integrity.empty()) integrity = lowerAscii(nestedStringValue(data, "_securityContext", "deviceIntegrity"));
    if (integrity == "fail" || integrity == "failed" || integrity == "compromised" ||
        integrity == "unsafe" || integrity == "low") {
        addRisk(score, signals, 85, "device_integrity_failed");
    } else if (integrity == "medium" || integrity == "unknown") {
        addRisk(score, signals, 25, "weak_device_integrity");
    }

    double reportedRisk = 0.0;
    if (numberValue(data, "riskScore", reportedRisk) ||
        nestedNumberValue(data, "security", "riskScore", reportedRisk) ||
        nestedNumberValue(data, "device", "riskScore", reportedRisk) ||
        nestedNumberValue(data, "_securityContext", "riskScore", reportedRisk)) {
        if (reportedRisk >= 80.0) {
            addRisk(score, signals, 80, "external_risk_score_high");
        } else if (reportedRisk >= 60.0) {
            addRisk(score, signals, 35, "external_risk_score_elevated");
        }
    }

    double trustScore = 100.0;
    if (numberValue(data, "deviceTrustScore", trustScore) ||
        nestedNumberValue(data, "security", "deviceTrustScore", trustScore) ||
        nestedNumberValue(data, "device", "deviceTrustScore", trustScore) ||
        nestedNumberValue(data, "_securityContext", "deviceTrustScore", trustScore)) {
        if (trustScore < 30.0) {
            addRisk(score, signals, 70, "device_trust_low");
        } else if (trustScore < 50.0) {
            addRisk(score, signals, 35, "device_trust_elevated");
        }
    }

    const std::string userAgent =
        nestedStringValue(data, "_securityContext", "userAgent");
    if (containsAny(userAgent, {"sqlmap", "nikto", "nmap", "masscan", "metasploit",
                                "nuclei", "acunetix", "nessus", "wpscan"})) {
        addRisk(score, signals, 90, "attack_tool_user_agent");
    } else if (containsAny(userAgent, {"burp", "owasp zap", "python-requests",
                                       "curl/", "wget/"})) {
        addRisk(score, signals, 55, "automation_user_agent");
    }

    const std::string forwardedFor =
        nestedStringValue(data, "_securityContext", "forwardedFor");
    if (!forwardedFor.empty() && forwardedFor.find(',') != std::string::npos) {
        addRisk(score, signals, 10, "multi_hop_forwarded_for");
    }

    std::size_t remainingChars = 4096;
    bool attackMarker = false;
    scanJsonForAttackMarkers(data, 0, remainingChars, attackMarker);
    if (attackMarker) {
        addRisk(score, signals, 75, "payload_attack_marker");
    }

    if ((channel == FalconChannel::MOBILE || channel == FalconChannel::RINGPAY) &&
        stringValue(data, "deviceId").empty()) {
        addRisk(score, signals, 15, "missing_device_id");
    }

    if (amount >= 100000.0) {
        addRisk(score, signals, 20, "very_large_amount");
    } else if (amount >= 50000.0) {
        addRisk(score, signals, 10, "large_amount");
    }

    const std::string signalText = joinSignals(signals);
    if (score >= FALCON_AI_DECLINE_SCORE) {
        reason = "Falcon AI security decline: high-risk device, network, or payload signal"
                 " detected (score=" + std::to_string(score) +
                 ", signals=" + signalText + ")";
        TransactionLogger::instance().logCurrent(
            "WARN", "falcon_ai_declined", "Falcon AI risk model declined transaction",
            {{"riskScore", std::to_string(score)}, {"riskSignals", signalText}});
        trace.fail("Falcon AI risk threshold exceeded",
                   {{"riskScore", std::to_string(score)},
                    {"riskSignals", signalText}});
        return true;
    }

    if (score > 0) {
        TransactionLogger::instance().logCurrent(
            "INFO", "falcon_ai_observed", "Falcon AI risk model observed signals",
            {{"riskScore", std::to_string(score)}, {"riskSignals", signalText}});
    }
    trace.success({{"riskScore", std::to_string(score)}, {"decision", "ALLOW"}});
    return false;
}

// ── Main check ────────────────────────────────────────────────────────────────
bool Falcon::checkFraud(std::string_view     accountNumber,
                        double               amount,
                        std::string&         reason,
                        FalconChannel        channel,
                        const nlohmann::json* requestData) {
    TransactionLogger::ScopedFunctionTrace trace("Falcon::checkFraud",
                                                 {{"fraudChannel", channelName(channel)},
                                                  {"amount", std::to_string(amount)}});
    TransactionLogger::instance().logCurrent(
        "INFO", "fraud_check_started", "Fraud rules evaluation started",
        {{"fraudChannel", channelName(channel)}, {"amount", std::to_string(amount)}});

    // ── AI security layer: device/network/payload threat signals ────────
    if (aiThreatScore_(requestData, amount, channel, reason)) {
        trace.fail("Falcon AI security rule matched",
                   {{"reason", reason}, {"rule", "falcon_ai_security"}});
        return true;
    }

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
                TransactionLogger::instance().logCurrent(
                    "WARN", "fraud_check_declined", reason,
                    {{"rule", "same_second"}, {"fraudChannel", channelName(channel)}});
                trace.fail("fraud rule matched", {{"rule", "same_second"}, {"reason", reason}});
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
                TransactionLogger::instance().logCurrent(
                    "WARN", "fraud_check_declined", reason,
                    {{"rule", "velocity"}, {"table", t.table}});
                trace.fail("fraud rule matched", {{"rule", "velocity"}, {"reason", reason}});
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
            TransactionLogger::instance().logCurrent(
                "WARN", "fraud_check_declined", reason,
                {{"rule", "cross_channel_velocity"}});
            trace.fail("fraud rule matched",
                       {{"rule", "cross_channel_velocity"}, {"reason", reason}});
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
        TransactionLogger::instance().logCurrent(
            "WARN", "fraud_check_declined", reason,
            {{"rule", "amount_spike"}, {"amount", std::to_string(amount)}});
        trace.fail("fraud rule matched", {{"rule", "amount_spike"}, {"reason", reason}});
        return true;
    }

    TransactionLogger::instance().logCurrent(
        "INFO", "fraud_check_passed", "Fraud rules evaluation passed",
        {{"fraudChannel", channelName(channel)}, {"amount", std::to_string(amount)}});
    trace.success({{"decision", "ALLOW"}});
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
    TransactionLogger::ScopedFunctionTrace trace("Falcon::logFraud",
                                                 {{"transactionId", std::string(txnId)}});
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

        TransactionLogger::instance().logCurrent(
            "WARN", "fraud_recorded", "Fraud decline recorded",
            {{"transactionId", std::string(txnId)},
             {"amount", std::to_string(amount)},
             {"reason", std::string(reason)}});
        trace.success({{"referenceId", std::to_string(id)}});

    } catch (const std::exception& ex) {
        std::cerr << "[Falcon] logFraud error: " << ex.what() << "\n";
        TransactionLogger::instance().logCurrent(
            "ERROR", "fraud_record_failed", "Failed to record fraud decline",
            {{"transactionId", std::string(txnId)}, {"error", ex.what()}});
        trace.fail("failed to record fraud decline", {{"error", ex.what()}});
    }
}
