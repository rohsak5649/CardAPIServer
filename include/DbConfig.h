/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * DB CONFIG LOADER  v1.0  — Encrypted, Tamper-Evident Database Credentials
 * ─────────────────────────────────────────────────────────────────────────
 * OVERVIEW
 *   Replaces hardcoded database credentials with a self-sealing config file.
 *
 *   ┌─ First Launch ─────────────────────────────────────────────────────┐
 *   │  1. User writes db.ini with plain-text credentials                 │
 *   │  2. DbConfig::load() reads it, encrypts it in-place (AES-256-GCM) │
 *   │  3. Application starts normally                                    │
 *   └────────────────────────────────────────────────────────────────────┘
 *   ┌─ Every Subsequent Launch ──────────────────────────────────────────┐
 *   │  1. DbConfig::load() reads the encrypted file                      │
 *   │  2. Verifies GCM authentication tag → tamper detection             │
 *   │  3. Decrypts and returns credentials to caller                     │
 *   └────────────────────────────────────────────────────────────────────┘
 *
 * ENCRYPTION
 *   Algorithm : AES-256-GCM (authenticated encryption — confidentiality
 *               AND integrity in a single pass, no separate HMAC needed)
 *   Key source: PBKDF2-HMAC-SHA256(machine_hostname, PEPPER, 200000 iter)
 *   Nonce     : 12-byte random IV generated fresh on every encryption
 *
 * TAMPER DETECTION
 *   AES-GCM produces a 16-byte authentication tag.  Any bit-flip in the
 *   ciphertext or the tag itself causes EVP_DecryptFinal to return -1.
 *   The loader converts this into a human-readable exception:
 *       std::runtime_error("DB config tampered — aborting startup")
 *
 * ENCRYPTED FILE FORMAT  (plain text lines, values hex-encoded)
 *   Line 1:  DBCFG_V1_ENC              ← magic / version sentinel
 *   Line 2:  <24 hex chars>            ← 12-byte random nonce
 *   Line 3:  <32 hex chars>            ← 16-byte GCM tag
 *   Line 4:  <N  hex chars>            ← ciphertext bytes
 *
 * PLAIN-TEXT INPUT FORMAT  (db.ini before first encryption)
 *   [database]
 *   host=localhost
 *   port=33060
 *   user=root
 *   pass=YourPasswordHere
 *   name=bankingdb
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#pragma once

#include <string>
#include <vector>
#include <stdexcept>

// ── Credentials bundle ────────────────────────────────────────────────────────
struct DbCredentials {
    std::string host;
    int         port   = 33060;
    std::string user;
    std::string pass;
    std::string dbName;
};

// ── DbConfig — public API ─────────────────────────────────────────────────────
class DbConfig {
public:
    /**
     * Load database credentials from @p path.
     *
     * Behaviour:
     *   • If the file starts with "DBCFG_V1_ENC" → decrypt and verify GCM tag.
     *     Throws std::runtime_error if the tag does not match (tamper detected).
     *   • Otherwise → treat as plain-text INI, parse it, then re-write the file
     *     as AES-256-GCM ciphertext so subsequent runs are encrypted.
     *
     * @param  path   Path to the config file (e.g. "db.ini")
     * @return DbCredentials struct populated with the loaded values
     * @throws std::runtime_error on file I/O errors, parse errors, or tampering
     */
    static DbCredentials load(const std::string& path);

private:
    // ── Internal helpers ──────────────────────────────────────────────────────
    static DbCredentials parsePlainText(const std::string& content);
    static std::string   encryptCredentials(const DbCredentials& creds);
    static DbCredentials decryptAndVerify(const std::string& content);
    static std::string   serializePlain(const DbCredentials& c);

    // Key derivation: PBKDF2(hostname, PEPPER, 200000) → 32-byte AES key
    static std::string   deriveKey();

    // Hex encode / decode helpers
    static std::string   toHex(const unsigned char* buf, size_t len);
    static std::vector<unsigned char> fromHex(const std::string& hex);

    // ── Compile-time constants ────────────────────────────────────────────────
    // Change PEPPER before production builds to make the key unique to your app.
    static constexpr const char* PEPPER       = "CardAPIServer-v1-rohan-secure-2026";
    static constexpr int         PBKDF2_ITER  = 200'000;
    static constexpr int         KEY_LEN      = 32;   // AES-256
    static constexpr int         NONCE_LEN    = 12;   // GCM standard
    static constexpr int         TAG_LEN      = 16;   // GCM tag
    static constexpr const char* MAGIC        = "DBCFG_V1_ENC";
};
