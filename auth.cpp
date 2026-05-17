#include "auth.h"
#include "TransactionLogger.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

using json = nlohmann::json;

const std::string AuthService::JWT_SECRET = "ROHAN_PAYMENT_ENGINE_JWT_SECRET_v3_9649";

AuthService& AuthService::instance() {
    static AuthService inst;
    return inst;
}

AuthService::AuthService() {}

// ── Base64URL helpers ────────────────────────────────────────────────────────
std::string AuthService::base64urlEncode(const std::string& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* buf = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, buf);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data.data(), (int)data.size());
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    // Convert to base64url: replace +→-, /→_, strip padding
    for (char& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

std::string AuthService::base64urlDecode(const std::string& input) {
    std::string data = input;
    for (char& c : data) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Re-pad
    while (data.size() % 4 != 0) data += '=';

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* buf = BIO_new_mem_buf(data.data(), (int)data.size());
    b64 = BIO_push(b64, buf);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    std::string result(data.size(), '\0');
    int len = BIO_read(b64, result.data(), (int)result.size());
    BIO_free_all(b64);
    result.resize(len > 0 ? len : 0);
    return result;
}

std::string AuthService::hmacSha256Hex(const std::string& data, const std::string& key) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hashLen = 0;
    HMAC(EVP_sha256(),
         key.data(),  (int)key.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         hash, &hashLen);
    // Return raw bytes as a string (not hex) for JWT signing
    return std::string(reinterpret_cast<char*>(hash), hashLen);
}

// ── API Key validation ───────────────────────────────────────────────────────
AuthContext AuthService::validateApiKey(const std::string& apiKey, mysqlx::Session& sess) const {
    if (apiKey.empty()) return {};
    try {
        auto res = sess.sql("SELECT role, owner_name, is_active FROM api_keys WHERE api_key = ?")
                       .bind(apiKey)
                       .execute();
        auto row = res.fetchOne();
        if (!row)             return {}; // key not found
        if ((int)row[2] == 0) return {}; // key inactive
        return {row[0].get<std::string>(), row[1].get<std::string>(), true};
    } catch (const std::exception& e) {
        TransactionLogger::instance().logCurrent("WARN", "auth_db_error", "DB error during API key lookup", {{"error", e.what()}});
        return {};
    }
}

// ── RBAC ─────────────────────────────────────────────────────────────────────
bool AuthService::checkPermission(const std::string& role, const std::string& channel) const {
    if (role == "ADMIN") return true;

    // MERCHANT: POS, ECOM, QR, RING, REVERSAL, account reads, 3DS
    static const std::unordered_set<std::string> merchantChannels = {
        "POS", "ECOM", "QRCODE", "RINGPAY", "REVERSAL",
        "ACCOUNT_DETAILS", "LIST_ACCOUNTS", "3DS_VERIFY", "3DS_INITIATE"
    };
    // TERMINAL: ATM, POS only
    static const std::unordered_set<std::string> terminalChannels = {
        "ATM", "POS"
    };
    // ISSUER_ROLE: card issuance + card management
    static const std::unordered_set<std::string> issuerChannels = {
        "ISSUER", "CARD_DETAILS",
        "CARD_ACTIVATE", "CARD_BLOCK", "CARD_SET_LIMIT",
        "ADD_ACCOUNT", "ACCOUNT_DETAILS", "LIST_ACCOUNTS",
        "FREEZE_ACCOUNT", "UNFREEZE_ACCOUNT"
    };
    // MOBILE_USER: mobile transfer, account view, pin reset
    static const std::unordered_set<std::string> mobileChannels = {
        "MOBILE", "ACCOUNT_DETAILS", "CARD_RESET_PIN"
    };

    if (role == "MERCHANT")    return merchantChannels.count(channel) > 0;
    if (role == "TERMINAL")    return terminalChannels.count(channel) > 0;
    if (role == "ISSUER_ROLE") return issuerChannels.count(channel) > 0;
    if (role == "MOBILE_USER") return mobileChannels.count(channel) > 0;
    return false;
}

// ── JWT generation ───────────────────────────────────────────────────────────
std::string AuthService::generateJWT(const AuthContext& ctx) const {
    long long now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    json payload = {
        {"sub", ctx.ownerName},
        {"role", ctx.role},
        {"iat", now},
        {"exp", now + 3600}  // 1 hour validity
    };

    std::string headerEnc  = base64urlEncode(header.dump());
    std::string payloadEnc = base64urlEncode(payload.dump());
    std::string signingInput = headerEnc + "." + payloadEnc;
    std::string sigRaw = hmacSha256Hex(signingInput, JWT_SECRET);
    std::string sigEnc = base64urlEncode(sigRaw);

    return headerEnc + "." + payloadEnc + "." + sigEnc;
}

// ── JWT validation ───────────────────────────────────────────────────────────
AuthContext AuthService::validateJWT(const std::string& token) const {
    try {
        // Split header.payload.signature
        auto dot1 = token.find('.');
        auto dot2 = token.find('.', dot1 + 1);
        if (dot1 == std::string::npos || dot2 == std::string::npos) return {};

        std::string headerEnc  = token.substr(0, dot1);
        std::string payloadEnc = token.substr(dot1 + 1, dot2 - dot1 - 1);
        std::string sigProvided = token.substr(dot2 + 1);

        // Verify signature
        std::string sigRaw = hmacSha256Hex(headerEnc + "." + payloadEnc, JWT_SECRET);
        std::string sigExpected = base64urlEncode(sigRaw);
        if (sigProvided != sigExpected) return {};

        // Decode payload
        std::string payloadJson = base64urlDecode(payloadEnc);
        json payload = json::parse(payloadJson);

        // Check expiry
        long long now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
        if (payload["exp"].get<long long>() < now) return {};

        return {payload["role"].get<std::string>(), payload["sub"].get<std::string>(), true};
    } catch (...) {
        return {};
    }
}
