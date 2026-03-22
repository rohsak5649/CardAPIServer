/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* This project involves complex transaction flow, fraud detection,
* and secure PAN encryption. Proper understanding is required before use.
*
* For support or implementation details:
* Email: rohanavinashsakhare@gmail.com
* Mobile: +91 9112765649
*/

#include "panencrypted.h"
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>
#include <cstring>
#include <stdexcept>

const int KEY_LEN = 32;
const int IV_LEN  = 12;
const int TAG_LEN = 16;

// 🔐 IMPORTANT: move this to config/env later
static const unsigned char AES_KEY[32] = {
    '1','2','3','4','5','6','7','8',
    '9','0','1','2','3','4','5','6',
    '7','8','9','0','1','2','3','4',
    '5','6','7','8','9','0','1','2'
};

// ================= SINGLETON =================
PANEncryptionService& PANEncryptionService::getInstance() {
    static PANEncryptionService instance;
    return instance;
}

// ================= BASE64 =================
std::string PANEncryptionService::base64_decode(const std::string& input) {
    BIO *b64, *bmem;
    char buffer[4096];

    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new_mem_buf(input.data(), input.length());
    bmem = BIO_push(b64, bmem);

    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);
    int len = BIO_read(bmem, buffer, sizeof(buffer));

    BIO_free_all(bmem);

    if (len <= 0)
        throw std::runtime_error("Base64 decode failed");

    return std::string(buffer, len);
}

// ================= DECRYPT =================
std::string PANEncryptionService::decryptPAN(const std::string& encoded) {
    std::string decoded = base64_decode(encoded);

    if (decoded.size() < IV_LEN + TAG_LEN)
        throw std::runtime_error("Invalid encrypted PAN");

    unsigned char iv[IV_LEN];
    unsigned char tag[TAG_LEN];

    memcpy(iv, decoded.data(), IV_LEN);
    memcpy(tag, decoded.data() + IV_LEN, TAG_LEN);

    unsigned char* ciphertext =
        (unsigned char*)decoded.data() + IV_LEN + TAG_LEN;

    int ciphertext_len = decoded.size() - IV_LEN - TAG_LEN;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();

    unsigned char plaintext[4096];
    int len, plaintext_len;

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL);
    EVP_DecryptInit_ex(ctx, NULL, NULL, AES_KEY, iv);

    EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len);
    plaintext_len = len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag);

    if (EVP_DecryptFinal_ex(ctx, plaintext + len, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("PAN decryption failed");
    }

    plaintext_len += len;
    EVP_CIPHER_CTX_free(ctx);

    return std::string((char*)plaintext, plaintext_len);
}

// ================= MASK =================
std::string PANEncryptionService::maskPAN(const std::string& pan) {
    if (pan.size() < 10) return pan;

    std::string masked = pan;
    for (size_t i = 4; i < pan.size() - 4; i++)
        masked[i] = '*';

    return masked;
}