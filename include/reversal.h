/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * REVERSAL ENGINE  v1.0  (C++20 · Thread-Safe · DB-Guard · Drop-File Fallback)
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 *   Handles automatic reversal of transactions that had money deducted but
 *   whose response was never received by the client due to network / timeout.
 *
 * FLOW
 *   1. Caller (channel handler) detects ERR_TIMEOUT / network loss AFTER a
 *      debit has already been committed to the DB.
 *   2. Caller invokes processReversal() with the original transaction details.
 *   3. processReversal() tries to:
 *        a. Acquire a DB connection (ping check).
 *        b. Credit back the deducted amount to the account.
 *        c. Record the reversal in  transaction_reversal  (channel table).
 *        d. Record the reversal in  transactions          (master registry).
 *        e. Mark the original channel row's reversal_status = 'REVERSED'.
 *   4. If the DB is unreachable the reversal payload is serialised to
 *      reversal_drop_file (persistent table) so a background retry worker
 *      can replay it once the DB recovers.
 *
 * NEW DB TABLES (see Create DB.sql)
 *   • transaction_reversal  — per-reversal detail rows
 *   • reversal_drop_file    — offline queue when DB is down
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#pragma once

#include "json.hpp"

using json = nlohmann::json;

// ── Public entry point ────────────────────────────────────────────────────────
// Called by any channel handler that detects a timeout / connectivity loss
// AFTER money has already been debited from the account.
//
// Required fields in `data`:
//   originalTransactionId  – transaction_id of the original debit row
//   originalTableName      – channel table name  (e.g. "transaction_atm")
//   originalReferenceId    – auto-increment id in that channel table
//   accountNumber          – account to credit back
//   amount                 – original deducted amount
//   fee                    – original deducted fee  (may be 0)
//   channel                – originating channel    (e.g. "ATM")
//   reason                 – human-readable reason  (e.g. "TIMEOUT")
//
// Optional fields:
//   clientTxnId            – echoed back in response
//   cardPan                – for card-based channels
//   cardScheme             – for card-based channels
//
[[nodiscard]] json processReversal(const json& data);
