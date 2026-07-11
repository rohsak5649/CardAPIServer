#pragma once

#include <mysqlx/xdevapi.h>
#include <string>
#include <optional>
#include <tuple>
#include <utility>

namespace DatabaseQueries {

    // 1. API Keys
    struct ApiKeyDetails {
        std::string role;
        std::string ownerName;
        bool isActive;
    };
    std::optional<ApiKeyDetails> getApiKeyDetails(mysqlx::Session& sess, const std::string& apiKey);

    // 2. Accounts
    struct AccountBalanceInfo {
        double balance;
        std::string currencyCode;
    };
    std::optional<AccountBalanceInfo> getAccountBalanceAndCurrency(mysqlx::Session& sess, const std::string& accountNo);
    std::optional<double> getAccountBalance(mysqlx::Session& sess, const std::string& accountNo, bool forUpdate = false);
    void updateAccountBalance(mysqlx::Session& sess, const std::string& accountNo, double newBalance);
    bool checkAccountExistsForUpdate(mysqlx::Session& sess, const std::string& accountNo);
    void updateFreezeStatus(mysqlx::Session& sess, const std::string& accountNo, bool frozen);

    // 3. Mobile
    double getMobileDailySpent(mysqlx::Session& sess, const std::string& debitAcc);
    double getMobileHourlySpent(mysqlx::Session& sess, const std::string& debitAcc);

    // 4. Idempotency Keys
    struct IdempotencyRecord {
        std::string status;
        int responseCode;
        std::string responseBody;
    };
    std::optional<IdempotencyRecord> getIdempotencyRecord(mysqlx::Session& sess, const std::string& key);
    void insertIdempotencyRecord(mysqlx::Session& sess, const std::string& key, const std::string& path);
    void updateIdempotencyRecord(mysqlx::Session& sess, const std::string& key, const std::string& status, int code, const std::string& body);

    // 5. 3D Secure Challenges
    struct ChallengeInfo {
        std::string otp;
        std::string transactionData;
        std::string status;
        long long expiresAt;
    };
    std::optional<ChallengeInfo> get3DSChallenge(mysqlx::Session& sess, const std::string& challengeId);
    void insert3DSChallenge(mysqlx::Session& sess, const std::string& challengeId, const std::string& otp, const std::string& txnData, const std::string& channel, int expiresSec = 300);
    void update3DSChallengeStatus(mysqlx::Session& sess, const std::string& challengeId, const std::string& status);

    // 6. Exchange Rates
    std::optional<double> getExchangeRate(mysqlx::Session& sess, const std::string& baseCurrency);

    // 7. ECOM Transaction Refunds
    struct EcomPurchaseInfo {
        int64_t id;
        double amount;
        double refundedAmount;
        std::string flag;
    };
    std::optional<EcomPurchaseInfo> getEcomPurchaseForUpdate(mysqlx::Session& sess, const std::string& clientTxnId);

    // 8. QR Transaction Refunds
    struct QrPurchaseInfo {
        int64_t id;
        double amount;
        std::string accountNumber;
    };
    std::optional<QrPurchaseInfo> getQrPurchaseForUpdate(mysqlx::Session& sess, const std::string& clientTxnId);
    bool checkQrAlreadyRefunded(mysqlx::Session& sess, int64_t purchaseId);

    // 9. Ledger Accounting
    int getOrCreateLedgerAccount(mysqlx::Session& sess, const std::string& accountNo, const std::string& defaultName, const std::string& type);
    void postLedgerEntry(mysqlx::Session& sess, const std::string& txnId, int ledgerId, double amount, const std::string& entryType, double balanceDelta, const std::string& description);

}
