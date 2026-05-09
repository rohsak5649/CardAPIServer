#pragma once
#include <string>
#include <optional>
#include <unordered_set>
#include "Database.h"
#include "json.hpp"

// ── Roles ────────────────────────────────────────────────────────────────────
// ADMIN        → all channels
// MERCHANT     → POS, ECOM, QRCODE, RINGPAY, ACCOUNT_DETAILS, REVERSAL
// TERMINAL     → ATM, POS
// ISSUER_ROLE  → ISSUER, CARD_DETAILS, card/activate, card/block, card/set_limit
// MOBILE_USER  → MOBILE, ACCOUNT_DETAILS, card/reset_pin
// ─────────────────────────────────────────────────────────────────────────────

struct AuthContext {
    std::string role;
    std::string ownerName;
    bool isValid = false;
};

class AuthService {
public:
    static AuthService& instance();

    // Validate X-API-Key header. Returns AuthContext (isValid=false if rejected).
    AuthContext validateApiKey(const std::string& apiKey, mysqlx::Session& sess) const;

    // Check whether a given role is allowed to access the given channel/path.
    bool checkPermission(const std::string& role, const std::string& channel) const;

    // Generate a JWT token for a validated API key holder (for stateless sessions).
    std::string generateJWT(const AuthContext& ctx) const;

    // Validate a JWT Bearer token. Returns AuthContext.
    AuthContext validateJWT(const std::string& token) const;

private:
    AuthService();
    static const std::string JWT_SECRET;

    // Base64URL encode/decode helpers
    static std::string base64urlEncode(const std::string& data);
    static std::string base64urlDecode(const std::string& data);
    static std::string hmacSha256Hex(const std::string& data, const std::string& key);
};
