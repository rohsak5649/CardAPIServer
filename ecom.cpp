#include "ecom.h"
#include "global_contant.h"
#include "DatabaseQueries.h"
#include "Database.h"
#include "falcon.h"
#include "panencrypted.h"
#include "pin.h"
#include "AccountLockManager.h"
#include "TransactionLogger.h"
#include "accounting.h"
#include "currency_converter.h"

#include <chrono>
#include <future>
#include <iostream>
#include <mysqlx/xdevapi.h>
#include <random>
#include <sstream>

using namespace mysqlx;
using json = nlohmann::json;

inline constexpr int ECOM_TIMEOUT_SEC = 5;

// ── TXN ID ─────────────────────────────────────────────

// ── ERROR HELPER ───────────────────────────────────────
static json err(const std::string &code, const std::string &msg = "") {
  json e{{"errorCode", code}};
  if (!msg.empty())
    e["message"] = msg;
  return e;
}


// ═══════════════════════════════════════════════════════
// CORE LOGIC
// ═══════════════════════════════════════════════════════
json processECOMTransactionCore(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processECOMTransactionCore",
                                               {{"transactionType", data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE))}});

  Database::ScopedConnection sc;
  Session &sess = *sc;
  Schema db = sess.getSchema("bankingdb");

  try {
    TransactionType txnType = stringToTransactionType(data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE)));
    double amount = data.value("amount", 0.0);
    std::string currency = data.value("currency", "INR");
    std::string clientTxnId = data.value("clientTxnId", TransactionLogger::currentUuid());

    // ── Resolve account number from card or direct input ──────────
    std::string accountNumber;
    std::string cardPan; // decrypted PAN (for logging/storage)
    std::string cardScheme;

    if (data.contains("card")) {
      // Card-based flow: decrypt PAN → verify PIN → lookup card → get account
      const json &card = data["card"];

      if (!card.contains("pan"))
      {
        trace.fail("missing card PAN");
        return err("ERR_MISSING_PAN", "Card PAN is required");
      }

      std::string encPan = card["pan"].get<std::string>();
      std::string expiry = card.value("expiry", "");

      // Decrypt PAN
      try {
        cardPan = PANEncryptionService::getInstance().decryptPAN(encPan);
      } catch (const std::exception &e) {
        trace.fail("encrypted PAN decrypt failed", {{"error", e.what()}});
        return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
      }

      // Verify PIN
      if (card.contains("pin")) {
        std::string pin = card["pin"].get<std::string>();
        if (!PINService::getInstance().verifyPIN(cardPan, pin)) {
          trace.fail("PIN verification failed");
          return err("ERR_INVALID_PIN", "PIN verification failed");
        }
      }

      // Card lookup
      Table cards = db.getTable("cards");
      auto cardRes = cards.select("account_number", "scheme", "status", "expiry", "cvv")
                         .where("pan = :p")
                         .bind("p", cardPan)
                         .execute();

      if (cardRes.count() == 0) {
        trace.fail("card not found");
        return err("ERR_CARD_NOT_FOUND", "Card not found");
      }

      Row cardRow = cardRes.fetchOne();
      accountNumber = cardRow[0].get<std::string>();
      cardScheme = cardRow[1].isNull() ? "" : cardRow[1].get<std::string>();
      std::string cardStatus = cardRow[2].get<std::string>();
      std::string dbExpiry = cardRow[3].get<std::string>();
      std::string dbCvv = cardRow[4].isNull() ? "" : cardRow[4].get<std::string>();

      if (!expiry.empty() && expiry != dbExpiry) {
        trace.fail("invalid expiry");
        return err("ERR_INVALID_EXPIRY", "Wrong expiry date");
      }

      std::string reqCvv = card.value("cvv", "");
      if (!reqCvv.empty() && reqCvv != dbCvv) {
        trace.fail("invalid CVV");
        return err("ERR_INVALID_CVV", "Wrong CVV");
      }

      if (cardStatus != "ACTIVE") {
        trace.fail("card inactive", {{"cardStatus", cardStatus}});
        return err("ERR_CARD_INACTIVE", "Card is not active");
      }

    } else {
      // Fallback: direct accountNumber (backward compatibility)
      accountNumber = data.value("accountNumber", "");
    }

    if (accountNumber.empty()) {
      trace.fail("missing account number");
      return err("ERR_MISSING_ACCOUNT", "accountNumber or card data required");
    }

    if (txnType == TransactionType::PURCHASE && amount <= 0) {
      trace.fail("invalid amount", {{"amount", std::to_string(amount)}});
      return err("ERR_INVALID_AMOUNT", "Amount must be > 0");
    }

    Table accounts = db.getTable("accounts");
    Table ecomTable = db.getTable("transaction_ecom");
    Table master = db.getTable("transactions");

    std::string merchantId = data.value("merchantId", "ECOM_MERCHANT");
    double fee = data.value("fee", 0.0);

    // accounts table has currency_id (FK), not currency column — must JOIN
    auto accInfo = DatabaseQueries::getAccountBalanceAndCurrency(sess, accountNumber);
    if (!accInfo) {
      trace.fail("account not found", {{"accountNumber", accountNumber}});
      return err("ERR_ACCOUNT_NOT_FOUND");
    }

    double balance = accInfo->balance;
    std::string accCurrency = accInfo->currencyCode.empty() ? "AUD" : accInfo->currencyCode;

    // Use the correlation UUID as the DB transaction_id — one ID for everything.
    std::string txnId = data.value("_correlationUuid", TransactionLogger::currentUuid());
    std::string scope =
        (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

    double origReqAmount = amount;
    double fxMarkup = 0.0;
    double fxRate = 1.0;
    
    if (accCurrency != currency) {
        auto fxRes = CurrencyConverter::convertToBase(currency, amount, sess);
        amount = fxRes.convertedAmount;
        fxMarkup = fxRes.bankMarkupAmount;
        fxRate = fxRes.fxRate;
        trace.success({{"convertedAmount", std::to_string(amount)}, {"fxRate", std::to_string(fxRate)}});
    }

    // ── FRAUD CHECK ───────────────────────────────
    Falcon falcon(sess);
    std::string reason;
    if (falcon.checkFraud(accountNumber, amount, reason, FalconChannel::ECOM,
                          &data)) {
      falcon.logFraud(txnId, clientTxnId, "", "", accountNumber, amount,
                      reason);
      trace.fail("fraud declined ECOM transaction", {{"reason", reason}});
      return {{"transactionId", txnId},
              {"status", "DECLINED"},
              {"message", reason}};
    }

    // ── PURCHASE ─────────────────────────────────
    if (txnType == TransactionType::PURCHASE) {
      AccountLockManager::ScopedLock accLock(
          AccountLockManager::getInstance(), accountNumber, TxnPriority::DEBIT);
      sess.startTransaction();
      try {
        auto currentBalOpt = DatabaseQueries::getAccountBalance(sess, accountNumber, true);
        if (!currentBalOpt) {
          sess.rollback();
          trace.fail("account not found", {{"accountNumber", accountNumber}});
          return err("ERR_ACCOUNT_NOT_FOUND");
        }
        double currentBalance = *currentBalOpt;

        if (currentBalance < amount + fee) {
          sess.rollback();
          trace.fail("insufficient funds",
                     {{"balance", std::to_string(currentBalance)},
                      {"amount", std::to_string(amount)}});
          return err("ERR_INSUFFICIENT_FUNDS");
        }

        double newBal = currentBalance - amount - fee;

        DatabaseQueries::updateAccountBalance(sess, accountNumber, newBal);

        auto ins =
            ecomTable
                .insert("transaction_id", "client_txn_id", "account_number",
                        "amount", "currency", "status", "message",
                        "transaction_scope", "merchant_id", "fee", "card_pan", "card_scheme")
                .values(txnId, clientTxnId, accountNumber, amount, currency,
                        "SUCCESS", "ECOM purchase success", scope, merchantId, fee, cardPan, cardScheme)
                .execute();

        master.insert("table_name", "reference_id", "status")
            .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
            .execute();

        Accounting::processPurchaseLedger(txnId, accountNumber, merchantId, amount, fee, fxMarkup, "ECOM Purchase", sess);
        sess.commit();

        trace.success({{"transactionId", txnId}, {"scope", scope}});
        return {{"transactionId", txnId},
                {"status", "SUCCESS"},
                {"balanceAfter", newBal},
                {"amountCharged", amount},
                {"scope", scope}};
      } catch (...) {
        sess.rollback();
        throw;
      }
    }

    // ── REFUND ───────────────────────────────────
    if (txnType == TransactionType::REFUND) {

      if (!data.contains("origClientTxnId")) {
        trace.fail("missing original client transaction id");
        return err("ERR_MISSING_ORIG_REF");
      }

      std::string orig = data["origClientTxnId"].get<std::string>();

      AccountLockManager::ScopedLock accLock(
          AccountLockManager::getInstance(), accountNumber, TxnPriority::CREDIT);

      sess.startTransaction();
      try {
        // Lock and check the original purchase row to prevent concurrent refunds
        auto pInfo = DatabaseQueries::getEcomPurchaseForUpdate(sess, orig);
        if (!pInfo) {
          sess.rollback();
          trace.fail("original ECOM purchase not found", {{"origClientTxnId", orig}});
          return err("ERR_ORIGINAL_NOT_FOUND");
        }

        int64_t refId = pInfo->id;
        double origAmt = pInfo->amount;
        double refAmt = pInfo->refundedAmount;
        std::string flag = pInfo->flag.empty() ? "N" : pInfo->flag;

        if (flag == "RF") {
          sess.rollback();
          trace.fail("already fully refunded");
          return err("ERR_ALREADY_REFUNDED");
        }

        if (amount + refAmt > origAmt) {
          sess.rollback();
          trace.fail("refund exceeds original amount",
                     {{"amount", std::to_string(amount)},
                      {"refundedAmount", std::to_string(refAmt)},
                      {"originalAmount", std::to_string(origAmt)}});
          return err("ERR_REFUND_EXCEEDS");
        }

        // Lock and read account balance
        auto balRes = DatabaseQueries::getAccountBalance(sess, accountNumber, true);
        if (!balRes) {
          sess.rollback();
          trace.fail("account not found for refund", {{"accountNumber", accountNumber}});
          return err("ERR_ACCOUNT_NOT_FOUND");
        }
        double currentBalance = *balRes;

        double newBal = currentBalance + amount;
        std::string newFlag = (amount + refAmt == origAmt) ? "RF" : "PR";

        DatabaseQueries::updateAccountBalance(sess, accountNumber, newBal);
            
        ecomTable.update()
            .set("refunded_amount", amount + refAmt)
            .set("flag", newFlag)
            .where("id=:id")
            .bind("id", refId)
            .execute();

        auto ins =
            ecomTable
                .insert("transaction_id", "client_txn_id", "account_number",
                        "amount", "currency", "status", "message", "reference_txn_id")
                .values(txnId, clientTxnId, accountNumber, amount, currency,
                        "SUCCESS", "ECOM refund success", orig)
                .execute();

        master.insert("table_name", "reference_id", "status")
            .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
            .execute();
            
        Accounting::processRefundLedger(txnId, accountNumber, merchantId, amount, "ECOM Refund", sess);
        sess.commit();

        trace.success({{"transactionId", txnId}, {"refundAmount", std::to_string(amount)}});
        return {{"transactionId", txnId},
                {"status", "SUCCESS"},
                {"flag", newFlag},
                {"balanceAfter", newBal}};
      } catch (...) {
        sess.rollback();
        throw;
      }
    }

    trace.fail("unsupported ECOM transaction type", {{"transactionType", transactionTypeToString(txnType)}});
    return err("ERR_INVALID_TYPE", "Unsupported transactionType");

  } catch (const std::exception &e) {
    trace.fail("ECOM core exception", {{"error", e.what()}});
    return err("ERR_EXCEPTION", e.what());
  }
}

// ═══════════════════════════════════════════════════════
// WRAPPER (TIMEOUT CONTROL)
// ═══════════════════════════════════════════════════════
json processECOMTransaction(const json &data) {

  TransactionLogger::ScopedFunctionTrace trace("processECOMTransaction",
                                               {{"transactionType", data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE))}});
  std::string uuid = data.value("_correlationUuid", TransactionLogger::currentUuid());
  TransactionLogger::ScopedContext scope(uuid, "ECOM");
  TransactionLogger::instance().logCurrent(
      "INFO", "channel_handler_scheduled",
      "ECOM transaction handler scheduled",
      {{"transactionType", data.value("transactionType", transactionTypeToString(TransactionType::PURCHASE))}});

  auto fut = std::async(std::launch::async, [data, uuid]() {
    TransactionLogger::ScopedContext asyncScope(uuid, "ECOM");
    TransactionLogger::instance().logCurrent(
        "INFO", "channel_handler_started",
        "ECOM transaction handler started",
        {{"transactionType", data.value("transactionType", "PURCHASE")}});
    return processECOMTransactionCore(data);
  });

  if (fut.wait_for(std::chrono::seconds(ECOM_TIMEOUT_SEC)) ==
      std::future_status::timeout) {
    TransactionLogger::instance().logCurrent(
        "WARN", "channel_handler_timeout",
        "ECOM transaction handler timeout",
        {{"timeoutSeconds", std::to_string(ECOM_TIMEOUT_SEC)}});
    trace.fail("ECOM transaction timeout");
    return {{"status", "FAILED"},
            {"errorCode", "ERR_TIMEOUT"},
            {"message", "ECOM transaction timeout"}};
  }

  json result = fut.get();
  TransactionLogger::instance().logCurrent(
      "INFO", "channel_handler_finished",
      "ECOM transaction handler finished",
      {{"transactionId", result.value("transactionId", "")},
       {"status", result.value("status", "")},
       {"errorCode", result.value("errorCode", "")}});
  if (result.contains("errorCode")) {
    trace.fail("ECOM transaction failed",
               {{"errorCode", result["errorCode"].get<std::string>()}});
  } else {
    trace.success({{"transactionId", result.value("transactionId", "")},
                   {"status", result.value("status", "")}});
  }
  return result;
}
