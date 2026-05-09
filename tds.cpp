#include "tds.h"
#include "Database.h"
#include "TransactionLogger.h"
#include "panencrypted.h"
#include "pin.h"
#include "ecom.h"
#include <mysqlx/xdevapi.h>
#include <chrono>
#include <random>
#include <sstream>

using namespace mysqlx;
using json = nlohmann::json;

static std::string genChallengeId() {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    std::mt19937 g{std::random_device{}()};
    std::ostringstream oss;
    oss << "3DS-" << ms << "-" << std::uniform_int_distribution<>(10000, 99999)(g);
    return oss.str();
}

static std::string genOTP() {
    std::mt19937 g{std::random_device{}()};
    int otp = std::uniform_int_distribution<>(100000, 999999)(g);
    return std::to_string(otp);
}

// ── Step 1: 3DS Challenge Initiation ─────────────────────────────────────────
// Called when channelId == "3DS_INITIATE" for ECOM purchases.
// Validates card/account, then parks transaction data and returns OTP challenge.
json processECOM3DSInitiate(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processECOM3DSInitiate");
    try {
        // Validate card exists and is ACTIVE
        if (!data.contains("card") || !data["card"].contains("pan")) {
            trace.fail("missing card/PAN");
            return {{"errorCode", "ERR_MISSING_PAN"}, {"message", "card.pan required"}};
        }

        std::string encPan = data["card"]["pan"].get<std::string>();
        std::string pan;
        try { pan = PANEncryptionService::getInstance().decryptPAN(encPan); }
        catch (const std::exception& e) {
            return {{"errorCode", "ERR_INVALID_ENCRYPTED_PAN"}, {"message", e.what()}};
        }

        Database::ScopedConnection sc;
        Session& sess = *sc;
        Schema db = sess.getSchema("bankingdb");

        auto cardRes = db.getTable("cards")
                          .select("status", "account_number", "expiry", "cvv")
                          .where("pan = :p").bind("p", pan).execute();
        if (cardRes.count() == 0) return {{"errorCode", "ERR_CARD_NOT_FOUND"}};

        Row r = cardRes.fetchOne();
        if (r[0].get<std::string>() != "ACTIVE") return {{"errorCode", "ERR_CARD_INACTIVE"}};

        std::string expiry  = data["card"].value("expiry", "");
        std::string reqCvv  = data["card"].value("cvv", "");
        if (!expiry.empty() && expiry != r[2].get<std::string>())
            return {{"errorCode", "ERR_INVALID_EXPIRY"}, {"message", "Wrong expiry"}};
        if (!reqCvv.empty() && reqCvv != (r[3].isNull() ? "" : r[3].get<std::string>()))
            return {{"errorCode", "ERR_INVALID_CVV"}, {"message", "Wrong CVV"}};

        // Generate OTP and store challenge
        std::string challengeId = genChallengeId();
        std::string otp = genOTP();

        // Store transaction_data (full data payload) for Step 2
        auto expires = std::chrono::system_clock::now() + std::chrono::minutes(10);
        long long expTs = std::chrono::duration_cast<std::chrono::seconds>(
                              expires.time_since_epoch()).count();

        sess.sql("INSERT INTO tds_challenges (challenge_id, otp, transaction_data, channel, expires_at) "
                 "VALUES (?, ?, ?, 'ECOM', FROM_UNIXTIME(?))")
            .bind(challengeId, otp, data.dump(), expTs)
            .execute();

        trace.success({{"challengeId", challengeId}});
        return {
            {"status",      "CHALLENGE_REQUIRED"},
            {"challengeId", challengeId},
            {"otpHint",     otp},   // In production this goes via SMS/email — here for testing
            {"redirectUrl", "http://localhost:8080/3ds/verify"},
            {"expiresInSeconds", 600},
            {"message",     "OTP sent to registered mobile number. Submit to /3ds/verify to complete payment."}
        };

    } catch (const std::exception& e) {
        trace.fail("3DS initiation exception", {{"error", e.what()}});
        return {{"errorCode", "ERR_EXCEPTION"}, {"message", e.what()}};
    }
}

// ── Step 2: OTP Verification & Payment Completion ────────────────────────────
json process3DSVerify(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("process3DSVerify");
    try {
        std::string challengeId = data.value("challengeId", "");
        std::string otp         = data.value("otp", "");

        if (challengeId.empty()) return {{"errorCode", "ERR_MISSING_CHALLENGE"}, {"message", "challengeId required"}};
        if (otp.empty())         return {{"errorCode", "ERR_MISSING_OTP"},       {"message", "otp required"}};

        Database::ScopedConnection sc;
        Session& sess = *sc;

        auto res = sess.sql("SELECT otp, transaction_data, status, expires_at FROM tds_challenges WHERE challenge_id = ?")
                       .bind(challengeId).execute();
        if (res.count() == 0) {
            trace.fail("challenge not found");
            return {{"errorCode", "ERR_CHALLENGE_NOT_FOUND"}, {"message", "Invalid challengeId"}};
        }

        Row row = res.fetchOne();
        std::string storedOtp    = row[0].get<std::string>();
        std::string txnDataStr   = row[1].get<std::string>();
        std::string status       = row[2].get<std::string>();

        if (status != "PENDING") {
            trace.fail("challenge already used", {{"status", status}});
            return {{"errorCode", "ERR_CHALLENGE_USED"}, {"message", "This challenge has already been used or expired"}};
        }

        // Check expiry
        long long now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
        long long expTs = (long long)row[3];
        if (now > expTs) {
            sess.sql("UPDATE tds_challenges SET status = 'EXPIRED' WHERE challenge_id = ?")
                .bind(challengeId).execute();
            return {{"errorCode", "ERR_OTP_EXPIRED"}, {"message", "OTP has expired. Please initiate a new payment."}};
        }

        if (otp != storedOtp) {
            trace.fail("invalid OTP");
            return {{"errorCode", "ERR_INVALID_OTP"}, {"message", "Invalid OTP"}};
        }

        // Mark challenge as VERIFIED
        sess.sql("UPDATE tds_challenges SET status = 'VERIFIED' WHERE challenge_id = ?")
            .bind(challengeId).execute();

        // Execute the actual ECOM transaction
        json txnData = json::parse(txnDataStr);
        json result  = processECOMTransaction(txnData);

        result["challengeId"]     = challengeId;
        result["threeDSStatus"]   = "AUTHENTICATED";
        trace.success({{"challengeId", challengeId}});
        return result;

    } catch (const std::exception& e) {
        trace.fail("3DS verify exception", {{"error", e.what()}});
        return {{"errorCode", "ERR_EXCEPTION"}, {"message", e.what()}};
    }
}
