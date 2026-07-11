#include "DatabaseQueries.h"
#include "TransactionLogger.h"
#include <mysqlx/xdevapi.h>
#include <chrono>

using namespace mysqlx;

namespace DatabaseQueries {

std::optional<ApiKeyDetails> getApiKeyDetails(Session& sess, const std::string& apiKey) {
    auto res = sess.sql("SELECT role, owner_name, is_active FROM api_keys WHERE api_key = ?")
                   .bind(apiKey)
                   .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    return ApiKeyDetails{
        row[0].get<std::string>(),
        row[1].get<std::string>(),
        (row[2].isNull() ? false : row[2].get<int>() != 0)
    };
}

std::optional<AccountBalanceInfo> getAccountBalanceAndCurrency(Session& sess, const std::string& accountNo) {
    auto res = sess.sql(
        "SELECT a.balance, c.currency_code FROM accounts a "
        "JOIN currency c ON c.currency_id = a.currency_id "
        "WHERE a.account_number = ?")
        .bind(accountNo)
        .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    return AccountBalanceInfo{
        row[0].get<double>(),
        row[1].get<std::string>()
    };
}

std::optional<double> getAccountBalance(Session& sess, const std::string& accountNo, bool forUpdate) {
    std::string query = "SELECT balance FROM accounts WHERE account_number = ?";
    if (forUpdate) {
        query += " FOR UPDATE";
    }
    auto res = sess.sql(query).bind(accountNo).execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    return row[0].get<double>();
}

void updateAccountBalance(Session& sess, const std::string& accountNo, double newBalance) {
    sess.sql("UPDATE accounts SET balance = ? WHERE account_number = ?")
        .bind(newBalance, accountNo)
        .execute();
}

bool checkAccountExistsForUpdate(Session& sess, const std::string& accountNo) {
    auto res = sess.sql("SELECT account_id FROM accounts WHERE account_number=? FOR UPDATE")
                   .bind(accountNo)
                   .execute();
    return res.count() > 0;
}

void updateFreezeStatus(Session& sess, const std::string& accountNo, bool frozen) {
    sess.sql("UPDATE accounts SET is_frozen=? WHERE account_number=?")
        .bind(frozen ? 1 : 0, accountNo)
        .execute();
}

double getMobileDailySpent(Session& sess, const std::string& debitAcc) {
    auto res = sess.sql("SELECT IFNULL(SUM(amount),0) FROM transaction_mobile "
                        "WHERE account_number=? AND status='SUCCESS' "
                        "AND DATE(created_at)=CURDATE()")
                   .bind(debitAcc)
                   .execute();
    if (res.count() == 0) return 0.0;
    auto row = res.fetchOne();
    if (!row) return 0.0;
    return row[0].get<double>();
}

double getMobileHourlySpent(Session& sess, const std::string& debitAcc) {
    auto res = sess.sql("SELECT IFNULL(SUM(amount),0) FROM transaction_mobile "
                        "WHERE account_number=? AND status='SUCCESS' "
                        "AND created_at >= NOW() - INTERVAL 1 HOUR")
                   .bind(debitAcc)
                   .execute();
    if (res.count() == 0) return 0.0;
    auto row = res.fetchOne();
    if (!row) return 0.0;
    return row[0].get<double>();
}

std::optional<IdempotencyRecord> getIdempotencyRecord(Session& sess, const std::string& key) {
    auto res = sess.sql("SELECT status, response_code, response_body FROM idempotency_keys WHERE idempotency_key = ?")
                   .bind(key)
                   .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    
    IdempotencyRecord rec;
    rec.status = row[0].get<std::string>();
    rec.responseCode = row[1].isNull() ? 0 : row[1].get<int>();
    rec.responseBody = row[2].isNull() ? "" : row[2].get<std::string>();
    return rec;
}

void insertIdempotencyRecord(Session& sess, const std::string& key, const std::string& path) {
    sess.sql("INSERT INTO idempotency_keys (idempotency_key, request_path, status) VALUES (?, ?, 'IN_PROGRESS')")
        .bind(key, path)
        .execute();
}

void updateIdempotencyRecord(Session& sess, const std::string& key, const std::string& status, int code, const std::string& body) {
    sess.sql("UPDATE idempotency_keys SET status = ?, response_code = ?, response_body = ? WHERE idempotency_key = ?")
        .bind(status, code, body, key)
        .execute();
}

std::optional<ChallengeInfo> get3DSChallenge(Session& sess, const std::string& challengeId) {
    auto res = sess.sql("SELECT otp, transaction_data, status, UNIX_TIMESTAMP(expires_at) FROM tds_challenges WHERE challenge_id = ?")
                   .bind(challengeId)
                   .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    
    ChallengeInfo info;
    info.otp = row[0].get<std::string>();
    info.transactionData = row[1].get<std::string>();
    info.status = row[2].get<std::string>();
    info.expiresAt = row[3].get<long long>();
    return info;
}

void insert3DSChallenge(Session& sess, const std::string& challengeId, const std::string& otp, const std::string& txnData, const std::string& channel, int expiresSec) {
    auto expires = std::chrono::system_clock::now() + std::chrono::seconds(expiresSec);
    long long expTs = std::chrono::duration_cast<std::chrono::seconds>(expires.time_since_epoch()).count();
    
    sess.sql("INSERT INTO tds_challenges (challenge_id, otp, transaction_data, channel, expires_at) "
             "VALUES (?, ?, ?, ?, FROM_UNIXTIME(?))")
        .bind(challengeId, otp, txnData, channel, expTs)
        .execute();
}

void update3DSChallengeStatus(Session& sess, const std::string& challengeId, const std::string& status) {
    sess.sql("UPDATE tds_challenges SET status = ? WHERE challenge_id = ?")
        .bind(status, challengeId)
        .execute();
}

std::optional<double> getExchangeRate(Session& sess, const std::string& baseCurrency) {
    auto res = sess.sql("SELECT rate FROM exchange_rates WHERE base_currency = ? AND target_currency = 'AUD'")
                   .bind(baseCurrency)
                   .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    return row[0].get<double>();
}

std::optional<EcomPurchaseInfo> getEcomPurchaseForUpdate(Session& sess, const std::string& clientTxnId) {
    auto res = sess.sql("SELECT id, amount, refunded_amount, flag FROM transaction_ecom "
                        "WHERE client_txn_id = ? AND message = 'ECOM purchase success' FOR UPDATE")
                   .bind(clientTxnId)
                   .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    
    EcomPurchaseInfo info;
    info.id = row[0].get<int64_t>();
    info.amount = row[1].get<double>();
    info.refundedAmount = row[2].isNull() ? 0.0 : row[2].get<double>();
    info.flag = row[3].isNull() ? "N" : row[3].get<std::string>();
    return info;
}

std::optional<QrPurchaseInfo> getQrPurchaseForUpdate(Session& sess, const std::string& clientTxnId) {
    auto res = sess.sql("SELECT id, amount, account_number FROM transaction_qrcode "
                        "WHERE client_txn_id = ? AND message = 'QR purchase successful' FOR UPDATE")
                   .bind(clientTxnId)
                   .execute();
    if (res.count() == 0) return std::nullopt;
    auto row = res.fetchOne();
    if (!row) return std::nullopt;
    
    QrPurchaseInfo info;
    info.id = row[0].get<int64_t>();
    info.amount = row[1].get<double>();
    info.accountNumber = row[2].get<std::string>();
    return info;
}

bool checkQrAlreadyRefunded(Session& sess, int64_t purchaseId) {
    auto res = sess.sql("SELECT id FROM transaction_qrcode "
                        "WHERE orig_ref_id = ? AND message = 'QR refund successful' FOR UPDATE")
                   .bind(purchaseId)
                   .execute();
    return res.count() > 0;
}

int getOrCreateLedgerAccount(Session& sess, const std::string& accountNo, const std::string& defaultName, const std::string& type) {
    sess.sql("INSERT IGNORE INTO ledger_accounts "
             "(account_number, account_name, account_type) VALUES (?, ?, ?)")
        .bind(accountNo, defaultName, type)
        .execute();

    auto res = sess.sql("SELECT ledger_id FROM ledger_accounts WHERE account_number = ?")
                   .bind(accountNo)
                   .execute();

    if (res.count() == 0) {
        throw std::runtime_error("ledger account not found after INSERT IGNORE for: " + accountNo);
    }
    return static_cast<int>(res.fetchOne()[0]);
}

void postLedgerEntry(Session& sess, const std::string& txnId, int ledgerId, double amount, const std::string& entryType, double balanceDelta, const std::string& description) {
    sess.sql("INSERT INTO ledger_entries "
             "(transaction_id, ledger_id, amount, entry_type, description) "
             "VALUES (?, ?, ?, ?, ?)")
        .bind(txnId, ledgerId, amount, entryType, description)
        .execute();

    sess.sql("UPDATE ledger_accounts SET balance = balance + ? WHERE ledger_id = ?")
        .bind(balanceDelta, ledgerId)
        .execute();
}

}
