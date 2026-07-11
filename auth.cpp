#include "auth.h"
#include "DatabaseQueries.h"
#include "TransactionLogger.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <memory>   // std::unique_ptr

using json = nlohmann::json;

// ── RAII guard for OpenSSL BIO chain ─────────────────────────────────────────
// Ensures BIO_free_all() is called on every code path — normal return,
// early-throw from a failed BIO_new/BIO_push, or any exception thereafter.
struct BioGuard {
    BIO* chain = nullptr;
    explicit BioGuard(BIO* b) : chain(b) {}
    ~BioGuard() { if (chain) BIO_free_all(chain); }
    // Non-copyable, non-movable — the guard owns the chain exclusively.
    BioGuard(const BioGuard&) = delete;
    BioGuard& operator=(const BioGuard&) = delete;
};


// JWT_SECRET is read from the environment variable JWT_SECRET_KEY at runtime.
// NEVER hardcode secrets in source — use a vault or env var in production.
static std::string loadJwtSecret() {
    const char* secret = std::getenv("JWT_SECRET_KEY");
    if (secret && std::strlen(secret) > 0) return std::string(secret);
    // Fallback for local dev only — will be overridden in production by env var.
    return "ROHAN_PAYMENT_ENGINE_JWT_SECRET_v3_9649";
}
const std::string AuthService::JWT_SECRET = loadJwtSecret();

AuthService& AuthService::instance() {
    static AuthService inst;
    return inst;
}

AuthService::AuthService() {}

// ── Base64URL helpers ──────────────────────────────────────────────────
std::string AuthService::base64urlEncode(const std::string& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* buf = BIO_new(BIO_s_mem());
    if (!b64 || !buf) {
        if (b64) BIO_free(b64);
        if (buf) BIO_free(buf);
        throw std::runtime_error("base64urlEncode: BIO allocation failed");
    }
    b64 = BIO_push(b64, buf);   // b64 now owns buf; free the whole chain via b64
    BioGuard guard(b64);        // RAII: frees chain on every exit path

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data.data(), static_cast<int>(data.size()));
    BIO_flush(b64);

    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    // guard destructor calls BIO_free_all(b64) here

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
    // Convert base64url back to standard base64
    for (char& c : data) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Re-pad to a multiple of 4
    while (data.size() % 4 != 0) data += '=';

    BIO* buf = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    if (!buf || !b64) {
        if (buf) BIO_free(buf);
        if (b64) BIO_free(b64);
        throw std::runtime_error("base64urlDecode: BIO allocation failed");
    }
    b64 = BIO_push(b64, buf);   // b64 now owns buf
    BioGuard guard(b64);        // RAII: frees chain on every exit path

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    // Decoded output is always ≤ 3/4 of the base64 input length.
    const std::size_t maxDecoded = (data.size() / 4) * 3 + 3;
    std::string result(maxDecoded, '\0');
    const int len = BIO_read(b64, result.data(), static_cast<int>(maxDecoded));
    // guard destructor calls BIO_free_all(b64) here

    result.resize(len > 0 ? static_cast<std::size_t>(len) : 0);
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
        auto details = DatabaseQueries::getApiKeyDetails(sess, apiKey);
        if (!details) return {}; // key not found
        if (!details->isActive) return {}; // key inactive
        return {details->role, details->ownerName, true};
    } catch (const std::exception& e) {
        TransactionLogger::instance().logCurrent("WARN", "auth_db_error", "DB error during API key lookup", {{"error", e.what()}});
        return {};
    }
}

// ── RBAC ─────────────────────────────────────────────────────────────────────
bool AuthService::checkPermission(const std::string& role, const std::string& channel) const {
    if (role == "ADMIN") return true;

    // MERCHANT: POS, ECOM, QR, RING, REVERSAL, account reads, 3DS, ICCW
    static const std::unordered_set<std::string> merchantChannels = {
        "POS", "ECOM", "QRCODE", "RINGPAY", "REVERSAL",
        "ACCOUNT_DETAILS", "LIST_ACCOUNTS", "3DS_VERIFY", "3DS_INITIATE", "ICCW"
    };
    // TERMINAL: ATM, POS, ICCW
    static const std::unordered_set<std::string> terminalChannels = {
        "ATM", "POS", "ICCW"
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
        // Use constant-time comparison to prevent timing attacks on the signature.
        // A naive string == comparison exits early on the first mismatch, allowing
        // an attacker to time many requests and deduce the correct signature.
        if (sigProvided.size() != sigExpected.size() ||
            CRYPTO_memcmp(sigProvided.data(), sigExpected.data(), sigExpected.size()) != 0) {
            return {};
        }

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
