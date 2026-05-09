# Payment Switching Engine v3.2

A C++20 banking and card payment switching engine that routes multiple digital payment channels through one HTTP API. The engine supports account management, card issuance, card lifecycle management, ATM, POS, ECOM, 3D Secure, mobile banking, QR payments, RingPay contactless payments, double-entry ledger accounting, dynamic currency conversion, partial refunds, idempotency keys, API key + JWT authentication, role-based access control, Falcon fraud monitoring, structured transaction logs, health checks, and Prometheus-style metrics.

## Table of Contents

- [Project Goals](#project-goals)
- [Core Features](#core-features)
- [Architecture](#architecture)
- [Authentication & RBAC](#authentication--rbac)
- [HTTP Endpoints](#http-endpoints)
- [Payment Channels](#payment-channels)
- [Advanced Financial Capabilities](#advanced-financial-capabilities)
- [3D Secure Flow](#3d-secure-flow)
- [Card Lifecycle Management](#card-lifecycle-management)
- [Idempotency](#idempotency)
- [Falcon Fraud Engine](#falcon-fraud-engine)
- [Security Model](#security-model)
- [Structured Logging](#structured-logging)
- [Database Design](#database-design)
- [Build and Run](#build-and-run)
- [Environment Variables](#environment-variables)
- [API Examples](#api-examples)
- [Error Codes](#error-codes)
- [Project Structure](#project-structure)
- [Production Notes](#production-notes)

## Project Goals

This project simulates the core responsibilities of a banking payment switch:

- Receive transaction requests from multiple channels.
- Authenticate callers via API keys or JWTs and enforce role-based access.
- Normalize requests into one routing format.
- Validate card, PIN, account, balance, limits, and fraud rules.
- Update account balances safely with double-entry ledger accounting.
- Support Dynamic Currency Conversion (DCC) with FX markup.
- Support partial and full refunds tracked per transaction.
- Prevent duplicate charges via idempotency key enforcement.
- Store channel-specific transaction records.
- Maintain a central master transaction registry.
- Return consistent JSON responses with request correlation IDs.
- Generate readable logs for operations, debugging, and audit tracing.

## Core Features

- **Unified router**: one `/transaction/initiate` endpoint routes by `channelId`.
- **API Key + JWT auth**: every endpoint requires `X-API-Key` or `Authorization: Bearer`.
- **RBAC**: five roles (ADMIN, MERCHANT, TERMINAL, ISSUER_ROLE, MOBILE_USER) with per-channel permissions.
- **Direct account APIs**: add, view, freeze, unfreeze, and list accounts.
- **Card lifecycle**: activate, block (temporary/permanent), set spending limits, reset PIN.
- **3D Secure mock flow**: two-step OTP challenge for ECOM payments.
- **Double-entry ledger**: every purchase/refund writes balanced DEBIT/CREDIT entries.
- **Dynamic Currency Conversion**: automatic FX lookup + 2% bank markup for cross-currency transactions.
- **Partial refunds**: multiple refunds against one purchase until fully refunded (flag: N → PR → RF).
- **Idempotency keys**: `Idempotency-Key` header prevents duplicate charges on retries.
- **Channel coverage**: ATM, Mobile, POS, ECOM, QRCode, RingPay, Issuer, Card Details.
- **Falcon fraud detection**: velocity checks, duplicate detection, amount spike, AI security scoring.
- **Threaded HTTP server**: 1000 worker threads with bounded queue back-pressure.
- **Database pool**: 30 MySQL X DevAPI sessions with stale-session healing.
- **Per-account locking**: prevents race conditions during concurrent credit/debit updates.
- **Request tracing**: every request receives `X-Request-ID` and `X-Transaction-UUID`.
- **Structured logs**: request summaries, function errors, line numbers, and self-healing log files.
- **Metrics and health**: `/health`, `/status`, and `/metrics` endpoints.
- **PAN encryption**: AES-256-GCM service for encrypted PAN transport.
- **PIN verification**: deterministic HMAC-SHA256 based PIN derivation, no PIN lookup table.

## Architecture

```
Client / Channel App
        │
        ▼
HTTP Router (parser&router.cpp)
        │
        ├── Auth Middleware (X-API-Key or Bearer JWT)
        ├── RBAC Check (role × channel permission matrix)
        ├── IP Rate Limiter (200 req/min per IP)
        ├── Idempotency Key Check
        ├── Request ID + Security Context
        │
        ▼
Channel Dispatch Map
        │
   ┌────┴────────────────────────────────────────────┐
   ATM   Mobile   POS   ECOM   QR   Ring   Issuer   Card Mgmt   3DS
        │
        ▼
Falcon Fraud Engine ──► transaction_falcon
        │
        ▼
DCC Engine (exchange_rates)
        │
        ▼
Account Lock Manager
        │
        ▼
MySQL bankingdb
  ├── ledger_entries (double-entry)
  ├── ledger_accounts (running balances)
  ├── channel transaction tables
  └── transactions (master registry)
        │
        ▼
Transaction Logger
```

## Authentication & RBAC

### How to Authenticate

Every endpoint requires one of:

```
X-API-Key: sk_admin_ROHAN_MASTER_9649
```

or a JWT obtained from `/auth/token`:

```
Authorization: Bearer <token>
```

### Get a JWT Token

```bash
curl -X POST http://localhost:8080/auth/token \
  -H 'X-API-Key: sk_admin_ROHAN_MASTER_9649'
```

Response:
```json
{
  "token": "eyJ...",
  "role": "ADMIN",
  "expiresIn": 3600,
  "tokenType": "Bearer"
}
```

### API Keys (Seed Data)

| Key | Role | Allowed Channels |
|-----|------|-----------------|
| `sk_admin_ROHAN_MASTER_9649` | ADMIN | All channels |
| `sk_merchant_POS_MCH001` | MERCHANT | POS, ECOM, QRCODE, RINGPAY, 3DS |
| `sk_terminal_ATM_4455` | TERMINAL | ATM, POS |
| `sk_issuer_BANK_CORE` | ISSUER_ROLE | ISSUER, CARD_*, account management |
| `sk_mobile_APP_USER1` | MOBILE_USER | MOBILE, CARD_RESET_PIN |

### RBAC Permission Matrix

| Channel | ADMIN | MERCHANT | TERMINAL | ISSUER_ROLE | MOBILE_USER |
|---------|-------|----------|----------|-------------|-------------|
| ATM | ✅ | ❌ | ✅ | ❌ | ❌ |
| POS | ✅ | ✅ | ✅ | ❌ | ❌ |
| ECOM | ✅ | ✅ | ❌ | ❌ | ❌ |
| MOBILE | ✅ | ❌ | ❌ | ❌ | ✅ |
| QRCODE/RINGPAY | ✅ | ✅ | ❌ | ❌ | ❌ |
| ISSUER/CARD_* | ✅ | ❌ | ❌ | ✅ | ❌ |
| CARD_RESET_PIN | ✅ | ❌ | ❌ | ❌ | ✅ |
| 3DS_INITIATE | ✅ | ✅ | ❌ | ❌ | ❌ |
| ADD_ACCOUNT | ✅ | ❌ | ❌ | ✅ | ❌ |

## HTTP Endpoints

| Method | Endpoint | Auth Role | Purpose |
|--------|----------|-----------|---------|
| `POST` | `/auth/token` | Any API Key | Exchange API key for JWT |
| `POST` | `/transaction/initiate` | Role by channel | Unified payment router |
| `POST` | `/account/add` | ISSUER_ROLE/ADMIN | Create account |
| `POST` | `/account/details` | MERCHANT+ | Fetch account |
| `POST` | `/account/freeze` | ISSUER_ROLE/ADMIN | Freeze account |
| `POST` | `/account/unfreeze` | ISSUER_ROLE/ADMIN | Unfreeze account |
| `POST` | `/account/list` | MERCHANT+ | List accounts |
| `POST` | `/card/activate` | ISSUER_ROLE/ADMIN | Activate card |
| `POST` | `/card/block` | ISSUER_ROLE/ADMIN | Block card (temp/permanent) |
| `POST` | `/card/set_limit` | ISSUER_ROLE/ADMIN | Set daily/monthly limits |
| `POST` | `/card/reset_pin` | MOBILE_USER/ADMIN | Reset card PIN |
| `POST` | `/3ds/verify` | MERCHANT/ADMIN | Verify 3DS OTP & complete payment |
| `POST` | `/reversal/initiate` | MERCHANT/ADMIN | Auto-reversal on timeout |
| `GET` | `/health` | None | Service health |
| `GET` | `/status` | None | Runtime counters |
| `GET` | `/metrics` | None | Prometheus-style metrics |

## Payment Channels

| Channel ID | Operations | Notes |
|------------|-----------|-------|
| `ATM` | WITHDRAWAL, DEPOSIT | Encrypted PAN, PIN, daily limits |
| `MOBILE` | FUND_TRANSFER | Hourly + daily limits, debit/credit accounts |
| `POS` | PURCHASE, REFUND | DCC, partial refunds, double-entry ledger |
| `ECOM` | PURCHASE, REFUND | DCC, partial refunds, double-entry ledger |
| `3DS_INITIATE` | Challenge | Returns OTP challenge for ECOM payments |
| `QRCODE` | PURCHASE, REFUND | QR merchant/terminal/currency parsing |
| `RINGPAY` | Contactless | Wearable token, merchant/daily limits, auto-reversal |
| `ISSUER` | Issue card | Luhn-valid PAN, expiry, CVV, encrypted PAN |
| `CARD_DETAILS` | Card lookup | Returns full card details from encrypted PAN |
| `CARD_ACTIVATE` | Activate | Re-activates a temporarily blocked card |
| `CARD_BLOCK` | Block | TEMPORARY (reversible) or PERMANENT |
| `CARD_SET_LIMIT` | Limits | Update dailyLimit and/or monthlyLimit |
| `CARD_RESET_PIN` | PIN | Verify new PIN against PAN-derived HMAC |

## Advanced Financial Capabilities

### Double-Entry Ledger Accounting

Every purchase and refund creates balanced ledger entries across four accounts:

**Purchase:**
- DEBIT: Customer Account (full amount)
- CREDIT: Merchant Account (net after fees)
- CREDIT: Bank Fee Revenue (flat fee)
- CREDIT: Bank FX Revenue (DCC markup, if applicable)

**Refund:**
- DEBIT: Merchant Account (refund amount)
- CREDIT: Customer Account (refund amount)

Tables: `ledger_accounts`, `ledger_entries`

### Dynamic Currency Conversion (DCC)

When the transaction currency differs from the account currency, the engine:
1. Looks up the FX rate from the `exchange_rates` table.
2. Converts the amount to the account's base currency.
3. Applies a 2% bank FX markup.
4. Records the FX markup as a separate ledger entry.

Supported currencies: USD, EUR, GBP, JPY, NZD, INR, AUD.

### Partial Refunds

Refunds are tracked cumulatively per original transaction:

| Flag | Meaning |
|------|---------|
| `N` | No refund |
| `PR` | Partial Refund (some amount refunded) |
| `RF` | Fully Refunded (cannot refund further) |

Attempting to refund beyond the original amount returns `ERR_REFUND_EXCEEDS`.

## 3D Secure Flow

ECOM payments can use a secure 2-step OTP challenge:

**Step 1 — Initiate:**
```json
POST /transaction/initiate
{ "channelId": "3DS_INITIATE", "data": { ... } }
```
Response: `{ "challengeId": "3DS-...", "otpHint": "492817", "expiresInSeconds": 600 }`

**Step 2 — Verify:**
```json
POST /3ds/verify
{ "challengeId": "3DS-...", "otp": "492817" }
```
Response: Same as a successful ECOM purchase + `"threeDSStatus": "AUTHENTICATED"`

OTPs expire after 10 minutes. Each challenge can only be used once.

## Card Lifecycle Management

| Endpoint | Body | Notes |
|----------|------|-------|
| `POST /card/activate` | `{ "encryptedPan": "..." }` | Cannot activate PERMANENT blocks |
| `POST /card/block` | `{ "encryptedPan": "...", "blockType": "TEMPORARY" }` | TEMPORARY or PERMANENT |
| `POST /card/set_limit` | `{ "encryptedPan": "...", "dailyLimit": 5000, "monthlyLimit": 50000 }` | Either field optional |
| `POST /card/reset_pin` | `{ "encryptedPan": "...", "expiry": "...", "newPin": "..." }` | Verifies PIN is PAN-derived |

## Idempotency

Send `Idempotency-Key: <uuid>` with any POST request to prevent duplicate processing:

```bash
curl -X POST http://localhost:8080/transaction/initiate \
  -H 'X-API-Key: sk_merchant_POS_MCH001' \
  -H 'Idempotency-Key: a1b2c3d4-...' \
  -H 'Content-Type: application/json' \
  -d '{ "channelId": "POS", "data": { ... } }'
```

On retry with the same key, the server returns the cached response immediately without reprocessing.

Table: `idempotency_keys`

## Falcon Fraud Engine

| Rule | Description |
|------|-------------|
| Same-second duplicate | Declines duplicate for same account within 1 second |
| Per-channel velocity | Declines if channel exceeds 5 transactions in 60 seconds |
| Cross-channel velocity | Detects rapid multi-channel activity |
| Amount spike | Declines if amount > 3× the 7-day average |
| Local AI security score | Scores device integrity, malware, proxy, VPN, and tamper signals |

Falcon declines at risk score ≥ 85. Declined events written to `transaction_falcon`.

### Security Headers for Falcon

`X-Device-Integrity`, `X-App-Signature-Valid`, `X-Device-Binding-Valid`, `X-Device-Trust-Score`, `X-Malware-Detected`, `X-Rooted-Device`, `X-Debugger-Detected`, `X-Proxy-Detected`, `X-VPN-Detected`, `X-Falcon-Risk-Score`

## Security Model

- **API Keys**: stored in `api_keys` table, looked up per request.
- **JWT**: HMAC-SHA256 signed, 1-hour validity, stateless verification.
- **PAN**: AES-256-GCM encrypted transport (12-byte IV, 16-byte tag, Base64).
- **PIN**: deterministic HMAC-SHA256 derivation from PAN — no stored PIN table.
- **Concurrency**: `AccountLockManager` serializes per-account updates; credits have priority over debits.
- **Transactions**: all balance updates wrapped in DB transactions with rollback on failure.

## Structured Logging

`TransactionLogger` writes to `bin/debug/log`. Records include: timestamp, level, UUID, channel, event, message, thread ID, function name, error source file and line. Logger auto-recovers when the log file is deleted.

```bash
export TRANSACTION_LOG_LEVEL=INFO   # TRACE DEBUG INFO WARN ERROR
```

## Database Design

| Table | Purpose |
|-------|---------|
| `accounts` | Balance, freeze status, country, currency |
| `currency` | Currency master |
| `cards` | PAN, encrypted PAN, scheme, expiry, CVV, priority, status, limits |
| `api_keys` | Authentication keys with role |
| `idempotency_keys` | Idempotency deduplication cache |
| `tds_challenges` | 3DS OTP challenges (10-min expiry) |
| `ledger_accounts` | Double-entry account balances |
| `ledger_entries` | DEBIT/CREDIT journal entries |
| `exchange_rates` | FX rates for DCC (USD, EUR, GBP, JPY, NZD, INR, AUD) |
| `transaction_atm` | ATM records |
| `transaction_mobile` | Mobile records |
| `transaction_pos` | POS records (with `refunded_amount`, `flag`) |
| `transaction_ecom` | ECOM records (with `refunded_amount`, `flag`) |
| `transaction_qrcode` | QR records |
| `transaction_ringpay` | RingPay records |
| `transaction_falcon` | Fraud decline records |
| `transactions` | Master registry |

## Build and Run

### Requirements

- C++20 compiler, CMake 3.15+
- MySQL Server with X Plugin enabled
- MySQL Connector/C++ 9.5 (X DevAPI)
- OpenSSL 3

### Build

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target BankingAPIServer -j 6
```

### Run

```bash
./cmake-build-debug/BankingAPIServer
```

Server listens on `http://0.0.0.0:8080`.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DB_HOST` | `localhost` | MySQL host |
| `DB_PORT` | `33060` | MySQL X Plugin port |
| `DB_USER` | `root` | Database user |
| `DB_PASS` | dev fallback | Database password |
| `DB_NAME` | `bankingdb` | Database name |
| `TRANSACTION_LOG_LEVEL` | `INFO` | Logger minimum level |

## API Examples

### Authenticate (Get JWT)

```bash
curl -X POST http://localhost:8080/auth/token \
  -H 'X-API-Key: sk_admin_ROHAN_MASTER_9649'
```

### POS Purchase with DCC

```bash
curl -X POST http://localhost:8080/transaction/initiate \
  -H 'X-API-Key: sk_merchant_POS_MCH001' \
  -H 'Content-Type: application/json' \
  -d '{
    "channelId": "POS",
    "data": {
      "clientTxnId": "pos-1001",
      "transactionType": "PURCHASE",
      "merchantId": "MCH-POS-001",
      "amount": 100.00, "fee": 2.00, "currency": "USD",
      "card": { "pan": "<encryptedPan>", "pin": "<pin>", "expiry": "<expiry>", "cvv": "<cvv>" }
    }
  }'
```

### ECOM 3DS Flow

```bash
# Step 1: Initiate
curl -X POST http://localhost:8080/transaction/initiate \
  -H 'X-API-Key: sk_merchant_POS_MCH001' \
  -H 'Content-Type: application/json' \
  -d '{ "channelId": "3DS_INITIATE", "data": { "amount": 250.00, "currency": "AUD", "card": { "pan": "<encryptedPan>", "expiry": "<expiry>", "cvv": "<cvv>" } } }'

# Step 2: Verify OTP
curl -X POST http://localhost:8080/3ds/verify \
  -H 'X-API-Key: sk_merchant_POS_MCH001' \
  -H 'Content-Type: application/json' \
  -d '{ "challengeId": "<from step1>", "otp": "<otpHint from step1>" }'
```

### Partial Refund

```bash
curl -X POST http://localhost:8080/transaction/initiate \
  -H 'X-API-Key: sk_merchant_POS_MCH001' \
  -H 'Content-Type: application/json' \
  -d '{
    "channelId": "POS",
    "data": {
      "transactionType": "REFUND",
      "origTransactionId": "<pos-txn-...>",
      "amount": 20.00,
      "card": { "pan": "<encryptedPan>", "pin": "<pin>", "expiry": "<expiry>" }
    }
  }'
```

### Block Card

```bash
curl -X POST http://localhost:8080/card/block \
  -H 'X-API-Key: sk_issuer_BANK_CORE' \
  -H 'Content-Type: application/json' \
  -d '{ "encryptedPan": "<encryptedPan>", "blockType": "TEMPORARY" }'
```

## Error Codes

| Code | Meaning |
|------|---------|
| `ERR_UNAUTHORIZED` | Missing/invalid API key or JWT |
| `ERR_FORBIDDEN` | Role not permitted for this channel |
| `ERR_CARD_NOT_FOUND` | Card PAN not found |
| `ERR_INVALID_EXPIRY` | Wrong expiry date |
| `ERR_INVALID_CVV` | Wrong CVV |
| `ERR_INVALID_ENCRYPTED_PAN` | PAN decryption failed |
| `ERR_INVALID_PIN` | PIN mismatch |
| `ERR_CARD_INACTIVE` | Card is BLOCKED |
| `ERR_CARD_PERMANENTLY_BLOCKED` | Cannot re-activate permanent block |
| `ERR_ACCOUNT_NOT_FOUND` | Account does not exist |
| `ERR_ACCOUNT_FROZEN` | Account is frozen |
| `ERR_INSUFFICIENT_FUNDS` | Balance too low |
| `ERR_ALREADY_REFUNDED` | Transaction fully refunded (flag=RF) |
| `ERR_REFUND_EXCEEDS` | Refund total exceeds original amount |
| `ERR_CHALLENGE_NOT_FOUND` | Invalid 3DS challengeId |
| `ERR_INVALID_OTP` | Wrong 3DS OTP |
| `ERR_OTP_EXPIRED` | 3DS OTP 10-minute window passed |
| `ERR_FX_RATE_NOT_FOUND` | No FX rate for currency pair |
| `ERR_FRAUD` | Falcon declined the transaction |
| `ERR_RATE_LIMIT` | >200 requests/min per IP |
| `ERR_TIMEOUT` | Channel processing exceeded timeout |

## Project Structure

```
.
├── parser&router.cpp       # HTTP server, routing, auth middleware, RBAC
├── auth.cpp                # API key validation, JWT generation/verification
├── card_mgmt.cpp           # Card lifecycle: activate, block, set_limit, reset_pin
├── tds.cpp                 # 3D Secure mock OTP challenge flow
├── accounting.cpp          # Double-entry ledger accounting
├── currency_converter.cpp  # Dynamic Currency Conversion + FX markup
├── idempotency.cpp         # Idempotency key deduplication
├── Database.cpp            # Self-healing MySQL X DevAPI connection pool
├── TransactionLogger.cpp   # Structured transaction logger
├── falcon.cpp              # Falcon fraud detection and AI risk scoring
├── account.cpp             # Account CRUD and freeze APIs
├── issue.cpp               # Card issuance and card details lookup
├── atm.cpp                 # ATM channel
├── mobile.cpp              # Mobile banking channel
├── pos.cpp                 # POS channel (DCC, partial refunds, ledger)
├── ecom.cpp                # ECOM channel (DCC, partial refunds, ledger)
├── qrcode.cpp              # QRCode channel
├── ringpay.cpp             # RingPay contactless channel
├── pin.cpp                 # PIN generation and HMAC verification
├── panencrypted.cpp        # AES-256-GCM PAN encryption/decryption
├── reversal.cpp            # Transaction reversal engine
├── include/                # Headers and bundled libraries
├── Create DB.sql           # Full database schema + seed data
├── API Document.txt        # Complete API reference
├── API Hit Tool            # Browser-based API testing UI (v3.2)
└── CMakeLists.txt          # Build configuration
```

## Production Notes

- Move AES key, PIN secret, and JWT secret to a vault or HSM.
- Replace plaintext `cards.pan` with tokenized or HSM-backed lookup.
- Restrict `CARD_DETAILS` channel to internal networks only.
- Enable TLS (HTTPS) at the load balancer or natively via OpenSSL.
- Replace seeded API keys with a key management service.
- Add database migrations (Flyway/Liquibase) instead of manual SQL edits.
- Add unit and integration tests for all channels and Falcon rules.
- Rotate JWT secret without downtime using a dual-key strategy.
- Ship structured JSON logs to a SIEM/observability platform (Elastic, Grafana Loki).
- Replace RingPay simulated network failure with real provider response handling.

## License

This repository currently does not include an open-source license file. Add a license before publishing or accepting external contributions.
