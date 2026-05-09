#include "accounting.h"
#include <stdexcept>

using namespace mysqlx;

static int getOrCreateLedgerAccount(const std::string& accountNumber, const std::string& defaultName, const std::string& type, Session& sess) {
    RowResult res = sess.sql("SELECT ledger_id FROM ledger_accounts WHERE account_number = ?")
        .bind(accountNumber)
        .execute();
    if (res.count() > 0) {
        return (int)res.fetchOne()[0];
    }
    
    sess.sql("INSERT INTO ledger_accounts (account_number, account_name, account_type) VALUES (?, ?, ?)")
        .bind(accountNumber, defaultName, type)
        .execute();
        
    RowResult res2 = sess.sql("SELECT ledger_id FROM ledger_accounts WHERE account_number = ?")
        .bind(accountNumber)
        .execute();
    return (int)res2.fetchOne()[0];
}

void Accounting::processPurchaseLedger(const std::string& transactionId, 
                                    const std::string& customerAccount,
                                    const std::string& merchantAccount,
                                    double totalAmount,
                                    double bankFee,
                                    double fxMarkup,
                                    const std::string& description,
                                    mysqlx::Session& sess) {
                                    
    int customerLedgerId = getOrCreateLedgerAccount(customerAccount, "Customer " + customerAccount, "LIABILITY", sess);
    int bankFeeLedgerId = getOrCreateLedgerAccount("BANK_FEE_REV", "Bank Fee Revenue", "REVENUE", sess);
    int bankFxLedgerId = getOrCreateLedgerAccount("BANK_FX_REV", "Bank FX Revenue", "REVENUE", sess);
    
    // Debit Customer
    sess.sql("INSERT INTO ledger_entries (transaction_id, ledger_id, amount, entry_type, description) VALUES (?, ?, ?, 'DEBIT', ?)")
        .bind(transactionId, customerLedgerId, -totalAmount, description)
        .execute();
    sess.sql("UPDATE ledger_accounts SET balance = balance - ? WHERE ledger_id = ?")
        .bind(totalAmount, customerLedgerId)
        .execute();
        
    // Credit Merchant
    double merchantAmount = totalAmount - bankFee - fxMarkup;
    if (merchantAmount > 0 && !merchantAccount.empty()) {
        int merchantLedgerId = getOrCreateLedgerAccount(merchantAccount, "Merchant " + merchantAccount, "LIABILITY", sess);
        sess.sql("INSERT INTO ledger_entries (transaction_id, ledger_id, amount, entry_type, description) VALUES (?, ?, ?, 'CREDIT', ?)")
            .bind(transactionId, merchantLedgerId, merchantAmount, description)
            .execute();
        sess.sql("UPDATE ledger_accounts SET balance = balance + ? WHERE ledger_id = ?")
            .bind(merchantAmount, merchantLedgerId)
            .execute();
    }
        
    // Credit Bank Fee
    if (bankFee > 0) {
        sess.sql("INSERT INTO ledger_entries (transaction_id, ledger_id, amount, entry_type, description) VALUES (?, ?, ?, 'CREDIT', ?)")
            .bind(transactionId, bankFeeLedgerId, bankFee, description)
            .execute();
        sess.sql("UPDATE ledger_accounts SET balance = balance + ? WHERE ledger_id = ?")
            .bind(bankFee, bankFeeLedgerId)
            .execute();
    }
    
    // Credit Bank FX
    if (fxMarkup > 0) {
        sess.sql("INSERT INTO ledger_entries (transaction_id, ledger_id, amount, entry_type, description) VALUES (?, ?, ?, 'CREDIT', ?)")
            .bind(transactionId, bankFxLedgerId, fxMarkup, description)
            .execute();
        sess.sql("UPDATE ledger_accounts SET balance = balance + ? WHERE ledger_id = ?")
            .bind(fxMarkup, bankFxLedgerId)
            .execute();
    }
}

void Accounting::processRefundLedger(const std::string& transactionId,
                                    const std::string& customerAccount,
                                    const std::string& merchantAccount,
                                    double refundAmount,
                                    const std::string& description,
                                    mysqlx::Session& sess) {
    int customerLedgerId = getOrCreateLedgerAccount(customerAccount, "Customer " + customerAccount, "LIABILITY", sess);
    
    // Credit Customer (give money back)
    sess.sql("INSERT INTO ledger_entries (transaction_id, ledger_id, amount, entry_type, description) VALUES (?, ?, ?, 'CREDIT', ?)")
        .bind(transactionId, customerLedgerId, refundAmount, description)
        .execute();
    sess.sql("UPDATE ledger_accounts SET balance = balance + ? WHERE ledger_id = ?")
        .bind(refundAmount, customerLedgerId)
        .execute();
        
    // Debit Merchant
    if (!merchantAccount.empty()) {
        int merchantLedgerId = getOrCreateLedgerAccount(merchantAccount, "Merchant " + merchantAccount, "LIABILITY", sess);
        sess.sql("INSERT INTO ledger_entries (transaction_id, ledger_id, amount, entry_type, description) VALUES (?, ?, ?, 'DEBIT', ?)")
            .bind(transactionId, merchantLedgerId, -refundAmount, description)
            .execute();
        sess.sql("UPDATE ledger_accounts SET balance = balance - ? WHERE ledger_id = ?")
            .bind(refundAmount, merchantLedgerId)
            .execute();
    }
}
