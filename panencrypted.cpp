/*
 * Copyright (c) Rohan Sakhare
 * All rights reserved.
 *
 * PAN ENCRYPTION & DECRYPTION SERVICE (AES-256-GCM)
 *
 * 1. PURPOSE:
 *    - Provides secure handling of Primary Account Number (PAN).
 *    - Ensures PAN is never exposed in plain text outside backend.
 *    - Used across:
 *        → ECOM transactions
 *        → ATM transactions
 *        → MOBILE transactions
 *
 * 2. DESIGN PATTERN:
 *    - Singleton pattern used for PANEncryptionService.
 *    - Ensures single shared instance across application.
 *    - Avoids repeated initialization overhead.
 *
 * 3. ENCRYPTION STANDARD:
 *    - Algorithm: AES-256-GCM (Authenticated Encryption)
 *    - Key length: 256 bits (32 bytes)
 *    - IV length: 12 bytes
 *    - Authentication tag: 16 bytes
 *
 *    SECURITY BENEFITS:
 *    - Confidentiality (data encryption)
 *    - Integrity (tamper detection via GCM tag)
 *    - Protection against replay/tampering attacks
 *
 * 4. INPUT FORMAT (ENCRYPTED PAN):
 *
 *    Base64 Encoded Payload Structure:
 *
 *        [ IV (12 bytes) | TAG (16 bytes) | CIPHERTEXT ]
 *
 *    - Entire payload encoded using Base64 before transmission.
 *
 * 5. DECRYPTION FLOW:
 *
 *    Step 1: Base64 decode input string
 *    Step 2: Extract:
 *        → IV (first 12 bytes)
 *        → TAG (next 16 bytes)
 *        → Ciphertext (remaining bytes)
 *
 *    Step 3: Initialize AES-256-GCM context
 *    Step 4: Perform decryption using key + IV
 *    Step 5: Validate authentication tag
 *
 *    - If tag validation fails → decryption rejected
 *    - Prevents tampered PAN usage
 *
 * 6. ERROR HANDLING:
 *    - Base64 decode failure → exception
 *    - Invalid payload structure → exception
 *    - GCM authentication failure → exception
 *
 * 7. MASKING FUNCTION:
 *    - maskPAN():
 *        → Masks middle digits of PAN
 *        → Example:
 *            5500004249929296 → 5500********9296
 *
 *    - Used for:
 *        → Logging
 *        → API responses
 *        → Debugging (safe display)
 *
 * 8. SECURITY NOTES (VERY IMPORTANT):
 *    - AES key is currently hardcoded (FOR DEMO ONLY)
 *
 *    ⚠ PRODUCTION REQUIREMENTS:
 *        → Store key in environment variables
 *        → Use secure vault (AWS Secrets Manager / HSM)
 *        → Never expose key in source code
 *
 *    - PAN should never be:
 *        → Logged in plain text
 *        → Stored unencrypted
 *        → Sent over insecure channels
 *
 * 9. PERFORMANCE NOTES:
 *    - Uses OpenSSL EVP API (optimized and secure)
 *    - Efficient for high-frequency transaction systems
 *
 * 10. FUTURE ENHANCEMENTS:
 *    - Add encryption function (currently only decrypt shown)
 *    - Key rotation support
 *    - Hardware Security Module (HSM) integration
 *    - Tokenization instead of PAN usage
 *
 * Unauthorized modification without understanding cryptographic
 * implications is strictly discouraged.
 *
 * For implementation details:
 * Email: rohanavinashsakhare@gmail.com
 * Mobile: +91 9112765649
 */
#include "panencrypted.h"
#include <cstring>
#include <memory>
#include <vector>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>

const int KEY_LEN = 32;
const int IV_LEN = 12;
const int TAG_LEN = 16;

// ── RAII wrapper for EVP_CIPHER_CTX ──────────────────────────────────────────
// Guarantees EVP_CIPHER_CTX_free() is called on every exit path:
// normal return, early throw, or any OpenSSL call failure.
using EvpCipherCtxPtr =
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

// 🔐 IMPORTANT: move this to config/env later
static const unsigned char AES_KEY[32] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1',
    '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2',
    '3', '4', '5', '6', '7', '8', '9', '0', '1', '2'};

// ================= SINGLETON =================
PANEncryptionService &PANEncryptionService::getInstance() {
  static PANEncryptionService instance;
  return instance;
}

// ================= BASE64 =================
std::string PANEncryptionService::base64_decode(const std::string &input) {
  // Decoded output is at most 3/4 of the base64 input — allocate exactly that
  // to avoid the silent truncation risk of a fixed 4096-byte stack buffer.
  const std::size_t maxDecoded = (input.size() / 4) * 3 + 3;
  std::vector<char> buffer(maxDecoded);

  BIO *b64  = BIO_new(BIO_f_base64());
  BIO *bmem = BIO_new_mem_buf(input.data(), static_cast<int>(input.length()));
  if (!b64 || !bmem) {
    if (b64)  BIO_free(b64);
    if (bmem) BIO_free(bmem);
    throw std::runtime_error("Base64 decode: BIO allocation failed");
  }
  bmem = BIO_push(b64, bmem); // b64 now owns bmem; free via BIO_free_all(bmem)

  BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
  int len = BIO_read(bmem, buffer.data(), static_cast<int>(buffer.size()));

  BIO_free_all(bmem);

  if (len <= 0)
    throw std::runtime_error("Base64 decode failed");

  return std::string(buffer.data(), static_cast<std::size_t>(len));
}

// ================= BASE64 ENCODE =================
std::string PANEncryptionService::base64_encode(const unsigned char *input,
                                                int length) {
  BIO *bmem, *b64;
  BUF_MEM *bptr;

  b64 = BIO_new(BIO_f_base64());
  bmem = BIO_new(BIO_s_mem());
  b64 = BIO_push(b64, bmem);

  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(b64, input, length);
  BIO_flush(b64);
  BIO_get_mem_ptr(b64, &bptr);

  std::string output(bptr->data, bptr->length);
  BIO_free_all(b64);

  return output;
}

// ================= ENCRYPT =================
std::string PANEncryptionService::encryptPAN(const std::string &plaintext) {
  unsigned char iv[IV_LEN];
  unsigned char tag[TAG_LEN];
  // ciphertext is at most plaintext.size() + one AES block (16 bytes)
  std::vector<unsigned char> ciphertext(plaintext.size() + 16);

  int len = 0, ciphertext_len = 0;

  RAND_bytes(iv, IV_LEN);

  // RAII: ctx is freed automatically on every exit path (return or exception)
  EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx)
    throw std::runtime_error("encryptPAN: failed to create EVP_CIPHER_CTX");

  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
    throw std::runtime_error("encryptPAN: EncryptInit failed");

  EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, IV_LEN, nullptr);
  if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, AES_KEY, iv) != 1)
    throw std::runtime_error("encryptPAN: key/IV init failed");

  if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &len,
                        reinterpret_cast<const unsigned char*>(plaintext.data()),
                        static_cast<int>(plaintext.size())) != 1)
    throw std::runtime_error("encryptPAN: EncryptUpdate failed");
  ciphertext_len = len;

  if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + len, &len) != 1)
    throw std::runtime_error("encryptPAN: EncryptFinal failed");
  ciphertext_len += len;

  EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag);
  // ctx freed here automatically by unique_ptr destructor

  // Combine: IV + TAG + CIPHERTEXT  (same format as offline tool)
  std::string combined(reinterpret_cast<char*>(iv), IV_LEN);
  combined.append(reinterpret_cast<char*>(tag), TAG_LEN);
  combined.append(reinterpret_cast<char*>(ciphertext.data()), ciphertext_len);

  return base64_encode(reinterpret_cast<unsigned char*>(combined.data()),
                       static_cast<int>(combined.size()));
}

// ================= DECRYPT =================
std::string PANEncryptionService::decryptPAN(const std::string &encoded) {
  std::string decoded = base64_decode(encoded);

  if (decoded.size() < static_cast<std::size_t>(IV_LEN + TAG_LEN))
    throw std::runtime_error("Invalid encrypted PAN");

  unsigned char iv[IV_LEN];
  unsigned char tag[TAG_LEN];

  memcpy(iv,  decoded.data(),           IV_LEN);
  memcpy(tag, decoded.data() + IV_LEN,  TAG_LEN);

  const unsigned char* ciphertext =
      reinterpret_cast<const unsigned char*>(decoded.data()) + IV_LEN + TAG_LEN;
  const int ciphertext_len = static_cast<int>(decoded.size()) - IV_LEN - TAG_LEN;

  // RAII: ctx freed automatically on every exit path — no manual free needed
  EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx)
    throw std::runtime_error("decryptPAN: failed to create EVP_CIPHER_CTX");

  // plaintext is at most as large as ciphertext
  std::vector<unsigned char> plaintext(static_cast<std::size_t>(ciphertext_len) + 16);
  int len = 0, plaintext_len = 0;

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
    throw std::runtime_error("decryptPAN: DecryptInit failed");
  EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, IV_LEN, nullptr);
  if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, AES_KEY, iv) != 1)
    throw std::runtime_error("decryptPAN: key/IV init failed");

  if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &len, ciphertext, ciphertext_len) != 1)
    throw std::runtime_error("decryptPAN: DecryptUpdate failed");
  plaintext_len = len;

  // Set GCM tag for authentication BEFORE calling DecryptFinal
  EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag);

  if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + len, &len) <= 0) {
    // ctx freed here automatically by unique_ptr — no manual call needed
    throw std::runtime_error("PAN decryption failed: GCM authentication tag mismatch");
  }
  plaintext_len += len;
  // ctx freed here automatically by unique_ptr destructor

  return std::string(reinterpret_cast<char*>(plaintext.data()),
                     static_cast<std::size_t>(plaintext_len));
}

// ================= MASK =================
std::string PANEncryptionService::maskPAN(const std::string &pan) {
  if (pan.size() < 10)
    return pan;

  std::string masked = pan;
  for (size_t i = 4; i < pan.size() - 4; i++)
    masked[i] = '*';

  return masked;
}