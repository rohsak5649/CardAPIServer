#include "pin.h"
#include <openssl/hmac.h>
#include <vector>
#include <stdexcept>

/* ===================== SINGLETON ACCESS ===================== */

PINService& PINService::getInstance() {
    static PINService instance; // thread-safe (C++11)
    return instance;
}

/* ===================== CONSTRUCTOR ===================== */

PINService::PINService()
    : secretKey("ULTRA_SECRET_KEY_2026_!@#") {}


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

        if (length == 0)
            throw std::invalid_argument("PIN length cannot be zero");

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