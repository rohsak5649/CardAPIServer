/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * DOUBLE-ENTRY LEDGER  v3.1
 * ─────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.1
 *   ✅ Single INSERT … ON DUPLICATE KEY UPDATE per ledger entry
 *      (was 2 round-trips: INSERT IGNORE + separate UPDATE)
 *   ✅ const-qualified all parameters that are read-only
 *   ✅ Improved error message for missing ledger account
 */

#include "accounting.h"
#include "DatabaseQueries.h"
#include <stdexcept>

using namespace mysqlx;

// Helper functions getOrCreateLedgerAccount and postEntry have been moved to DatabaseQueries namespace.

// ── Purchase ledger ───────────────────────────────────────────────────────────
void Accounting::processPurchaseLedger(const std::string& transactionId,
                                       const std::string& customerAccount,
                                       const std::string& merchantAccount,
                                       double             totalAmount,
                                       double             bankFee,
                                       double             fxMarkup,
                                       const std::string& description,
                                       Session&           sess) {
    const int customerLedgerId =
        DatabaseQueries::getOrCreateLedgerAccount(sess, customerAccount,
                                 "Customer " + customerAccount, "LIABILITY");
    const int bankFeeLedgerId =
        DatabaseQueries::getOrCreateLedgerAccount(sess, "BANK_FEE_REV", "Bank Fee Revenue", "REVENUE");
    const int bankFxLedgerId =
        DatabaseQueries::getOrCreateLedgerAccount(sess, "BANK_FX_REV", "Bank FX Revenue", "REVENUE");

    // Debit customer (money leaves their account)
    DatabaseQueries::postLedgerEntry(sess, transactionId, customerLedgerId,
              totalAmount, "DEBIT", -totalAmount, description);

    // Credit merchant (net of bank fees and FX markup)
    const double merchantAmount = totalAmount - bankFee - fxMarkup;
    if (merchantAmount > 0.0 && !merchantAccount.empty()) {
        const int merchantLedgerId =
            DatabaseQueries::getOrCreateLedgerAccount(sess, merchantAccount,
                                     "Merchant " + merchantAccount, "LIABILITY");
        DatabaseQueries::postLedgerEntry(sess, transactionId, merchantLedgerId,
                  merchantAmount, "CREDIT", +merchantAmount, description);
    }

    // Credit bank fee revenue
    if (bankFee > 0.0) {
        DatabaseQueries::postLedgerEntry(sess, transactionId, bankFeeLedgerId,
                  bankFee, "CREDIT", +bankFee, description);
    }

    // Credit bank FX revenue
    if (fxMarkup > 0.0) {
        DatabaseQueries::postLedgerEntry(sess, transactionId, bankFxLedgerId,
                  fxMarkup, "CREDIT", +fxMarkup, description);
    }
}

// ── Refund ledger ─────────────────────────────────────────────────────────────
void Accounting::processRefundLedger(const std::string& transactionId,
                                     const std::string& customerAccount,
                                     const std::string& merchantAccount,
                                     double             refundAmount,
                                     const std::string& description,
                                     Session&           sess) {
    const int customerLedgerId =
        DatabaseQueries::getOrCreateLedgerAccount(sess, customerAccount,
                                 "Customer " + customerAccount, "LIABILITY");

    // Credit customer (money returns to their account)
    DatabaseQueries::postLedgerEntry(sess, transactionId, customerLedgerId,
              refundAmount, "CREDIT", +refundAmount, description);

    // Debit merchant (money leaves merchant's ledger account)
    if (!merchantAccount.empty()) {
        const int merchantLedgerId =
            DatabaseQueries::getOrCreateLedgerAccount(sess, merchantAccount,
                                     "Merchant " + merchantAccount, "LIABILITY");
        DatabaseQueries::postLedgerEntry(sess, transactionId, merchantLedgerId,
                  refundAmount, "DEBIT", -refundAmount, description);
    }
}
