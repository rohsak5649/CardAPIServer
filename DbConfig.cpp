/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * DB CONFIG LOADER — IMPLEMENTATION  v1.0
 * ─────────────────────────────────────────────────────────────────────────────
 * Implements DbConfig::load() which provides:
 *   • Plain-text INI parsing on first run
 *   • AES-256-GCM encryption of credentials in-place (machine-locked key)
 *   • Automatic decryption and GCM tag verification on subsequent runs
 *   • Tamper detection: any bit-flip in the file → startup abort
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "DbConfig.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif

// Platform-specific hostname and executable path headers
#ifdef _WIN32
#  include <winsock2.h>      // GetComputerNameA (gethostname is in ws2_32)
#  include <windows.h>       // GetModuleFileNameA
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <unistd.h>        // gethostname (POSIX)
#endif

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>


// ─────────────────────────────────────────────────────────────────────────────
// Hex helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string DbConfig::toHex(const unsigned char* buf, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<unsigned>(buf[i]);
    return oss.str();
}

std::vector<unsigned char> DbConfig::fromHex(const std::string& hex) {
    if (hex.size() % 2 != 0)
        throw std::runtime_error("[DbConfig] Invalid hex string (odd length)");
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned int byte = 0;
        std::istringstream ss(hex.substr(i, 2));
        ss >> std::hex >> byte;
        out.push_back(static_cast<unsigned char>(byte));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Key Derivation — PBKDF2-HMAC-SHA256
//   Salt  = machine hostname (makes key machine-specific)
//   Info  = PEPPER (compiled-in, makes key app-specific)
//   Iter  = 200 000 rounds
// ─────────────────────────────────────────────────────────────────────────────

std::string DbConfig::deriveKey() {
    // Get machine hostname as the salt component
    char hostBuf[256] = {};

#ifdef _WIN32
    // Windows: use GetComputerNameA (no WSAStartup needed for this)
    DWORD hostLen = sizeof(hostBuf);
    if (!GetComputerNameA(hostBuf, &hostLen))
        throw std::runtime_error("[DbConfig] GetComputerNameA() failed");
#else
    // POSIX (macOS / Linux)
    if (gethostname(hostBuf, sizeof(hostBuf)) != 0)
        throw std::runtime_error("[DbConfig] gethostname() failed");
#endif

    std::string hostname(hostBuf);

    // Combine hostname + pepper as the actual PBKDF2 salt
    std::string salt = hostname + "::" + PEPPER;

    unsigned char key[KEY_LEN] = {};
    int rc = PKCS5_PBKDF2_HMAC(
        PEPPER,           static_cast<int>(std::strlen(PEPPER)),  // password
        reinterpret_cast<const unsigned char*>(salt.c_str()),
        static_cast<int>(salt.size()),                             // salt
        PBKDF2_ITER,
        EVP_sha256(),
        KEY_LEN,
        key
    );
    if (rc != 1)
        throw std::runtime_error("[DbConfig] PBKDF2 key derivation failed");

    return std::string(reinterpret_cast<char*>(key), KEY_LEN);
}

// ─────────────────────────────────────────────────────────────────────────────
// Plain-text INI → DbCredentials
//   Accepts lines like:  host=localhost
//   Section headers ([database]) are silently skipped.
// ─────────────────────────────────────────────────────────────────────────────

DbCredentials DbConfig::parsePlainText(const std::string& content) {
    DbCredentials creds;
    std::istringstream ss(content);
    std::string line;
    bool anyField = false;

    while (std::getline(ss, line)) {
        // Strip trailing \r (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip blank lines and section headers
        if (line.empty() || line[0] == '[' || line[0] == '#' || line[0] == ';')
            continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim whitespace from key and value
        auto trim = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c){ return !std::isspace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
        };
        trim(key); trim(val);

        if      (key == "host") { creds.host   = val; anyField = true; }
        else if (key == "port") { creds.port   = std::stoi(val); anyField = true; }
        else if (key == "user") { creds.user   = val; anyField = true; }
        else if (key == "pass") { creds.pass   = val; anyField = true; }
        else if (key == "name") { creds.dbName = val; anyField = true; }
    }

    if (!anyField)
        throw std::runtime_error("[DbConfig] db.ini parsed but no recognised fields found");
    if (creds.host.empty())   throw std::runtime_error("[DbConfig] db.ini missing 'host'");
    if (creds.user.empty())   throw std::runtime_error("[DbConfig] db.ini missing 'user'");
    if (creds.pass.empty())   throw std::runtime_error("[DbConfig] db.ini missing 'pass'");
    if (creds.dbName.empty()) throw std::runtime_error("[DbConfig] db.ini missing 'name'");

    return creds;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialize credentials to plain INI text (used before encryption)
// ─────────────────────────────────────────────────────────────────────────────

std::string DbConfig::serializePlain(const DbCredentials& c) {
    std::ostringstream oss;
    oss << "[database]\n"
        << "host=" << c.host   << "\n"
        << "port=" << c.port   << "\n"
        << "user=" << c.user   << "\n"
        << "pass=" << c.pass   << "\n"
        << "name=" << c.dbName << "\n";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// AES-256-GCM Encrypt
//   Returns the DBCFG_V1_ENC file content as a string (4 lines).
// ─────────────────────────────────────────────────────────────────────────────

std::string DbConfig::encryptCredentials(const DbCredentials& creds) {
    std::string key = deriveKey();
    std::string plaintext = serializePlain(creds);

    // Generate random 12-byte nonce
    unsigned char nonce[NONCE_LEN];
    if (RAND_bytes(nonce, NONCE_LEN) != 1)
        throw std::runtime_error("[DbConfig] RAND_bytes failed — cannot generate nonce");

    // Prepare output buffer (same size as plaintext; GCM has no padding)
    std::vector<unsigned char> ciphertext(plaintext.size());
    unsigned char tag[TAG_LEN] = {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("[DbConfig] EVP_CIPHER_CTX_new failed");

    try {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            throw std::runtime_error("[DbConfig] EVP_EncryptInit_ex (alg) failed");

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, nullptr) != 1)
            throw std::runtime_error("[DbConfig] EVP_CIPHER_CTX_ctrl (IV len) failed");

        if (EVP_EncryptInit_ex(ctx,
                nullptr,
                nullptr,
                reinterpret_cast<const unsigned char*>(key.data()),
                nonce) != 1)
            throw std::runtime_error("[DbConfig] EVP_EncryptInit_ex (key/iv) failed");

        int outLen = 0;
        if (EVP_EncryptUpdate(ctx,
                ciphertext.data(), &outLen,
                reinterpret_cast<const unsigned char*>(plaintext.data()),
                static_cast<int>(plaintext.size())) != 1)
            throw std::runtime_error("[DbConfig] EVP_EncryptUpdate failed");

        int finalLen = 0;
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen, &finalLen) != 1)
            throw std::runtime_error("[DbConfig] EVP_EncryptFinal_ex failed");

        ciphertext.resize(outLen + finalLen);

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) != 1)
            throw std::runtime_error("[DbConfig] EVP_CIPHER_CTX_ctrl (get tag) failed");

    } catch (...) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }
    EVP_CIPHER_CTX_free(ctx);

    // Build the 4-line file format
    std::ostringstream out;
    out << MAGIC << "\n"
        << toHex(nonce, NONCE_LEN) << "\n"
        << toHex(tag, TAG_LEN) << "\n"
        << toHex(ciphertext.data(), ciphertext.size()) << "\n";

    return out.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// AES-256-GCM Decrypt + GCM tag verification
//   Throws runtime_error with "tampered" message on auth failure.
// ─────────────────────────────────────────────────────────────────────────────

DbCredentials DbConfig::decryptAndVerify(const std::string& content) {
    std::istringstream ss(content);
    std::string magic, nonceHex, tagHex, cipherHex;

    if (!std::getline(ss, magic)     || magic != MAGIC)
        throw std::runtime_error("[DbConfig] Unrecognised file format");
    if (!std::getline(ss, nonceHex)  || nonceHex.size() != size_t(NONCE_LEN * 2))
        throw std::runtime_error("[DbConfig] Corrupt nonce in config file");
    if (!std::getline(ss, tagHex)    || tagHex.size() != size_t(TAG_LEN * 2))
        throw std::runtime_error("[DbConfig] Corrupt GCM tag in config file");
    if (!std::getline(ss, cipherHex) || cipherHex.empty())
        throw std::runtime_error("[DbConfig] Corrupt ciphertext in config file");

    // Strip trailing \r just in case
    auto stripCR = [](std::string& s){ if (!s.empty() && s.back()=='\r') s.pop_back(); };
    stripCR(nonceHex); stripCR(tagHex); stripCR(cipherHex);

    auto nonce      = fromHex(nonceHex);
    auto tag        = fromHex(tagHex);
    auto ciphertext = fromHex(cipherHex);

    std::string key = deriveKey();

    std::vector<unsigned char> plaintext(ciphertext.size());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("[DbConfig] EVP_CIPHER_CTX_new failed");

    try {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            throw std::runtime_error("[DbConfig] EVP_DecryptInit_ex (alg) failed");

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NONCE_LEN, nullptr) != 1)
            throw std::runtime_error("[DbConfig] EVP_CIPHER_CTX_ctrl (IV len) failed");

        if (EVP_DecryptInit_ex(ctx,
                nullptr, nullptr,
                reinterpret_cast<const unsigned char*>(key.data()),
                nonce.data()) != 1)
            throw std::runtime_error("[DbConfig] EVP_DecryptInit_ex (key/iv) failed");

        int outLen = 0;
        if (EVP_DecryptUpdate(ctx,
                plaintext.data(), &outLen,
                ciphertext.data(),
                static_cast<int>(ciphertext.size())) != 1)
            throw std::runtime_error("[DbConfig] EVP_DecryptUpdate failed");

        // Set the expected GCM tag before calling Final
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                TAG_LEN,
                const_cast<unsigned char*>(tag.data())) != 1)
            throw std::runtime_error("[DbConfig] EVP_CIPHER_CTX_ctrl (set tag) failed");

        int finalLen = 0;
        int rc = EVP_DecryptFinal_ex(ctx, plaintext.data() + outLen, &finalLen);
        EVP_CIPHER_CTX_free(ctx);
        ctx = nullptr;

        if (rc <= 0) {
            // ⚠ GCM authentication tag mismatch = TAMPERED or wrong machine
            throw std::runtime_error(
                "[DbConfig] *** SECURITY ALERT: DB config file has been TAMPERED or "
                "moved to a different machine — authentication tag mismatch. "
                "Startup aborted to protect database credentials. ***");
        }

        plaintext.resize(outLen + finalLen);

    } catch (...) {
        if (ctx) EVP_CIPHER_CTX_free(ctx);
        throw;
    }

    std::string decrypted(reinterpret_cast<char*>(plaintext.data()), plaintext.size());
    return parsePlainText(decrypted);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: resolve the config file from multiple candidate directories
// ─────────────────────────────────────────────────────────────────────────────

// Extract just the filename from a path (e.g. "cfg/db.ini" → "db.ini")
static std::string basename_(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// Return the first path in 'candidates' that can be opened for reading.
// Returns "" if none found.
static std::string findFile_(const std::vector<std::string>& candidates) {
    for (const auto& p : candidates) {
        std::ifstream f(p);
        if (f.is_open()) return p;
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point: DbConfig::load()
// ─────────────────────────────────────────────────────────────────────────────

DbCredentials DbConfig::load(const std::string& path) {
    const std::string fname = basename_(path);

    // ── Search candidate locations (in priority order) ────────────────────────
    // 1. Exactly as given (works when CWD == project root, or absolute path)
    // 2. ../db.ini  — covers CLion default: binary in cmake-build-debug/,
    //                 config in project root one level up
    // 3. ../../db.ini — covers nested build dirs (cmake-build-debug/Debug/ etc.)
    std::vector<std::string> candidates = {
        path,
        "../"  + fname,
        "../../" + fname,
    };

    // 4. macOS: directory next to the running executable
#ifdef __APPLE__
    {
        char exePath[4096] = {};
        uint32_t size = sizeof(exePath);
        if (_NSGetExecutablePath(exePath, &size) == 0) {
            std::string ep(exePath);
            auto slash = ep.find_last_of('/');
            if (slash != std::string::npos)
                candidates.push_back(ep.substr(0, slash + 1) + fname);
        }
    }
#endif

    std::string resolvedPath = findFile_(candidates);

    if (resolvedPath.empty()) {
        // Build a human-readable list of tried paths for the error message
        std::string tried;
        for (const auto& c : candidates) tried += "\n    • " + c;
        throw std::runtime_error(
            "[DbConfig] Could not find '" + fname + "'. Tried:" + tried + "\n\n"
            "  Create '" + fname + "' in the project root (or next to the binary)\n"
            "  with your database credentials:\n\n"
            "    [database]\n"
            "    host=localhost\n"
            "    port=33060\n"
            "    user=root\n"
            "    pass=YOUR_PASSWORD\n"
            "    name=bankingdb\n\n"
            "  The file will be auto-encrypted on first startup.");
    }

    std::cout << "[DbConfig] Found config at '" << resolvedPath << "'\n";

    // ── Read the resolved file ────────────────────────────────────────────────
    std::ifstream ifs(resolvedPath);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // ── Determine mode: encrypted or plain text ───────────────────────────────
    bool isEncrypted = (content.size() >= std::strlen(MAGIC) &&
                        content.substr(0, std::strlen(MAGIC)) == MAGIC);

    if (isEncrypted) {
        // ── Already encrypted: decrypt + verify ──────────────────────────────
        std::cout << "[DbConfig] Loading encrypted config…\n";
        DbCredentials creds = decryptAndVerify(content);
        std::cout << "[DbConfig] ✅ Credentials verified and loaded successfully.\n";
        return creds;

    } else {
        // ── Plain text: parse → encrypt → overwrite ───────────────────────────
        std::cout << "[DbConfig] Plain-text config detected.\n"
                  << "[DbConfig] Encrypting with AES-256-GCM (machine-locked key)…\n";

        DbCredentials creds = parsePlainText(content);
        std::string encrypted = encryptCredentials(creds);

        // Overwrite the file atomically-ish: write to .tmp then rename
        std::string tmpPath = resolvedPath + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::trunc | std::ios::binary);
            if (!ofs.is_open())
                throw std::runtime_error("[DbConfig] Cannot write temporary file: " + tmpPath);
            ofs << encrypted;
        }

        if (std::rename(tmpPath.c_str(), resolvedPath.c_str()) != 0) {
            // Fallback: direct overwrite (rename can fail cross-device)
            std::ofstream ofs(resolvedPath, std::ios::trunc | std::ios::binary);
            if (!ofs.is_open())
                throw std::runtime_error("[DbConfig] Cannot overwrite '" + resolvedPath + "' with encrypted version");
            ofs << encrypted;
            std::remove(tmpPath.c_str());
        }

        std::cout << "[DbConfig] ✅ Config encrypted successfully. "
                  << "Plain-text credentials replaced with ciphertext.\n"
                  << "[DbConfig] ⚠  Keep a backup of your password — "
                  << "the plain-text version no longer exists on disk.\n";

        return creds;
    }
}

