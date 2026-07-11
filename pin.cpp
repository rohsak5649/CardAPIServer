/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* PIN SERVICE – SECURE PIN GENERATION & VERIFICATION ENGINE
*
* 1. PURPOSE:
*    - Provides secure PIN generation and verification mechanism.
*    - Eliminates need to store PINs in database.
*    - Ensures deterministic, tamper-resistant PIN validation.
*
* 2. DESIGN PATTERN:
*    - Singleton pattern used for PINService.
*    - Ensures single shared instance across application.
*    - Thread-safe initialization (C++11 static instance).
*
* 3. CORE CONCEPT:
*    - PIN is NOT stored anywhere.
*    - PIN is derived dynamically using:
*        → PAN (Primary Account Number)
*        → Secret Key
*        → Cryptographic transformations
*
*    - Verification:
*        → Re-generate PIN from PAN
*        → Compare with input PIN
*
* 4. CRYPTOGRAPHIC FLOW:
*
*    Step 1: HMAC-SHA256
*        - Input: PAN / evolving state
*        - Key: Secret key (server-side only)
*        - Output: 256-bit hash
*
*    Step 2: Compression
*        - Hash reduced to 64-bit value
*        - Uses:
*            → Bit rotation (rotl)
*            → Avalanche function (strong mixing)
*
*    Step 3: Multi-round Derivation
*        - 6 rounds of transformation
*        - Each round feeds into next state
*        - Produces multiple entropy blocks
*
*    Step 4: Final PIN Generation
*        - Combines derived blocks
*        - Generates PIN using character pool:
*            → Uppercase letters
*            → Lowercase letters
*            → Digits
*            → Special characters
*
*        - Ensures complexity:
*            → At least 1 uppercase
*            → At least 1 lowercase
*            → At least 1 digit
*            → At least 1 special character
*
* 5. PIN VERIFICATION:
*
*    - Input PIN received from client
*    - System regenerates PIN using PAN
*    - Compares generated PIN with input
*
*    - If match → VALID
*    - Else → INVALID
*
* 6. SECURITY ADVANTAGES:
*
*    - No PIN storage (eliminates DB leakage risk)
*    - Deterministic verification (no lookup needed)
*    - Resistant to:
*        → Replay attacks
*        → Database compromise
*        → Brute-force attacks (high entropy)
*
* 7. KEY MANAGEMENT (CRITICAL):
*
*    - Secret key used for HMAC:
*        → "ULTRA_SECRET_KEY_2026_!@#"
*
*    ⚠ PRODUCTION REQUIREMENTS:
*        → Move key to environment variables
*        → Use secure vault / HSM
*        → Rotate keys periodically
*
* 8. ERROR HANDLING:
*
*    - Empty input → exception
*    - Invalid PAN → exception
*    - HMAC failure → exception
*    - Internal failures wrapped with context
*
* 9. PERFORMANCE:
*
*    - Lightweight computation (no DB calls)
*    - Suitable for high-frequency transaction systems
*    - Constant-time operations (low latency)
*
* 10. FUTURE ENHANCEMENTS:
*
*    - Add rate limiting for PIN attempts
*    - Add device/IP binding for extra security
*    - Add PIN retry lock mechanism
*    - Integrate with hardware security module (HSM)
*
* DESIGN NOTES:
*    - Combines cryptographic hashing + custom mixing
*    - Inspired by secure key derivation techniques
*    - Ensures strong randomness and unpredictability
*
* SECURITY WARNING:
*    - Any modification in logic will break PIN consistency
*    - Must maintain backward compatibility for existing users
*
* Unauthorized modification without understanding
* cryptographic derivation logic is strictly discouraged.
*
* For implementation details:
* Email: rohanavinashsakhare@gmail.com
* Mobile: +91 9112765649
*/

#include "pin.h"
#include <openssl/hmac.h>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdlib>

/* ===================== SINGLETON ACCESS ===================== */

PINService& PINService::getInstance() {
    static PINService instance; // thread-safe (C++11)
    return instance;
}

/* ===================== CONSTRUCTOR ===================== */

PINService::PINService() {
    // Read secret key from environment variable for production security.
    // NEVER hardcode HMAC keys in source — use a vault or env var.
    const char* envKey = std::getenv("PIN_SECRET_KEY");
    secretKey = (envKey && std::strlen(envKey) > 0)
                ? std::string(envKey)
                : "ULTRA_SECRET_KEY_2026_!@#";  // fallback for local dev only
}


/* ===================== BIT MIX ===================== */

unsigned long long PINService::rotl(unsigned long long x, int r) const {
    return (x << r) | (x >> (64 - r));
}

unsigned long long PINService::avalanche(unsigned long long x) const {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}


/* ===================== HMAC ===================== */

std::vector<unsigned char> PINService::hmacSha256(const std::string& data) const {
    try {
        unsigned int len = 32;
        std::vector<unsigned char> result(len);

        if (secretKey.empty()) {
            throw std::runtime_error("Secret key is empty");
        }

        if (data.empty()) {
            throw std::invalid_argument("Input data is empty");
        }

        unsigned char* res = HMAC(
            EVP_sha256(),
            secretKey.data(), secretKey.size(),
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(),
            result.data(), &len
        );

        if (!res) {
            throw std::runtime_error("HMAC generation failed");
        }

        return result;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("hmacSha256 failed: ") + e.what());
    }
}


/* ===================== DERIVATION ===================== */

unsigned long long PINService::compress(const std::vector<unsigned char>& v) const {
    try {
        unsigned long long acc = 0;

        for (size_t i = 0; i < v.size(); i++) {
            acc ^= (unsigned long long)v[i] << ((i % 8) * 8);
            acc = rotl(acc, 13);
            acc = avalanche(acc);
        }

        return acc;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("compress failed: ") + e.what());
    }
}

std::vector<unsigned long long> PINService::derive(const std::string& pan) const {
    try {
        std::vector<unsigned long long> blocks;

        std::string state = pan;

        for (int i = 0; i < 6; i++) {
            auto d = hmacSha256(state);
            unsigned long long val = compress(d);

            blocks.push_back(val);
            state = std::to_string(val) + state;
        }

        return blocks;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("derive failed: ") + e.what());
    }
}


/* ===================== FINAL PIN ===================== */

std::string PINService::generatePIN(const std::string& pan, size_t length) const {
    try {
        if (pan.size() != 16)
            throw std::invalid_argument("PAN must be 16 digits");

        if (length < 4)
            throw std::invalid_argument("PIN length must be at least 4");

        auto blocks = derive(pan);

        std::string result;
        result.reserve(length);

        unsigned long long state = 0;

        for (auto b : blocks) {
            state ^= avalanche(b);
        }

        const std::string pool =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789"
            "!@#$%^&*()-_=+[]{}|;:,.<>?/";

        for (size_t i = 0; i < length; i++) {
            state = rotl(state ^ (i * 0x9e3779b97f4a7c15ULL), 7);
            state = avalanche(state);

            result += pool[state % pool.size()];
        }

        // enforce categories
        result[0] = 'A' + (state % 26);
        result[1] = 'a' + ((state >> 3) % 26);
        result[2] = '0' + ((state >> 5) % 10);
        result[3] = "!@#$%^&*"[state % 8];

        return result;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("generatePIN failed: ") + e.what());
    }
}


/* ===================== VERIFY ===================== */

bool PINService::verifyPIN(const std::string& pan, const std::string& inputPin) const {
    try {
        if (inputPin.empty())
            throw std::invalid_argument("Input PIN cannot be empty");

        std::string generated = generatePIN(pan, inputPin.length());
        return generated == inputPin;
    }
    catch (const std::exception& e) {
        throw std::runtime_error(std::string("verifyPIN failed: ") + e.what());
    }
}