#include "ecom.h"
#include "Database.h"
#include "falcon.h"
#include "panencrypted.h"
#include "pin.h"

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
static std::string genECOMTxnId() {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  std::mt19937 g{std::random_device{}()};
  return "ecom-txn-" + std::to_string(ms) + "-" +
         std::to_string(std::uniform_int_distribution<>(1000, 9999)(g));
}

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

  Database::ScopedConnection sc;
  Session &sess = *sc;
  Schema db = sess.getSchema("bankingdb");

  try {
    std::string txnType = data.value("transactionType", "PURCHASE");
    double amount = data.value("amount", 0.0);
    std::string currency = data.value("currency", "INR");
    std::string clientTxnId = data.value("clientTxnId", genECOMTxnId());

    // ── Resolve account number from card or direct input ──────────
    std::string accountNumber;
    std::string cardPan; // decrypted PAN (for logging/storage)
    std::string cardScheme;

    if (data.contains("card")) {
      // Card-based flow: decrypt PAN → verify PIN → lookup card → get account
      const json &card = data["card"];

      if (!card.contains("pan"))
        return err("ERR_MISSING_PAN", "Card PAN is required");

      std::string encPan = card["pan"].get<std::string>();
      std::string expiry = card.value("expiry", "");

      // Decrypt PAN
      try {
        cardPan = PANEncryptionService::getInstance().decryptPAN(encPan);
      } catch (const std::exception &e) {
        return err("ERR_INVALID_ENCRYPTED_PAN", e.what());
      }

      // Verify PIN
      if (card.contains("pin")) {
        std::string pin = card["pin"].get<std::string>();
        if (!PINService::getInstance().verifyPIN(cardPan, pin))
          return err("ERR_INVALID_PIN", "PIN verification failed");
      }

      // Card lookup
      Table cards = db.getTable("cards");
      auto cardRes = cards.select("account_number", "scheme", "status", "expiry", "cvv")
                         .where("pan = :p")
                         .bind("p", cardPan)
                         .execute();

      if (cardRes.count() == 0)
        return err("ERR_CARD_NOT_FOUND", "Card not found");

      Row cardRow = cardRes.fetchOne();
      accountNumber = cardRow[0].get<std::string>();
      cardScheme = cardRow[1].isNull() ? "" : cardRow[1].get<std::string>();
      std::string cardStatus = cardRow[2].get<std::string>();
      std::string dbExpiry = cardRow[3].get<std::string>();
      std::string dbCvv = cardRow[4].isNull() ? "" : cardRow[4].get<std::string>();

      if (!expiry.empty() && expiry != dbExpiry) {
        return err("ERR_INVALID_EXPIRY", "Wrong expiry date");
      }

      std::string reqCvv = card.value("cvv", "");
      if (!reqCvv.empty() && reqCvv != dbCvv) {
        return err("ERR_INVALID_CVV", "Wrong CVV");
      }

      if (cardStatus != "ACTIVE")
        return err("ERR_CARD_INACTIVE", "Card is not active");

    } else {
      // Fallback: direct accountNumber (backward compatibility)
      accountNumber = data.value("accountNumber", "");
    }

    if (accountNumber.empty())
      return err("ERR_MISSING_ACCOUNT", "accountNumber or card data required");

    if (txnType == "PURCHASE" && amount <= 0)
      return err("ERR_INVALID_AMOUNT", "Amount must be > 0");

    Table accounts = db.getTable("accounts");
    Table ecomTable = db.getTable("transaction_ecom");
    Table master = db.getTable("transactions");

    auto accRes = accounts.select("balance", "currency")
                      .where("account_number=:a")
                      .bind("a", accountNumber)
                      .execute();

    if (accRes.count() == 0)
      return err("ERR_ACCOUNT_NOT_FOUND");

    Row accRow = accRes.fetchOne();
    double balance = accRow[0].get<double>();
    std::string accCurrency = accRow[1].get<std::string>();

    std::string txnId = genECOMTxnId();
    std::string scope =
        (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

    // ── FRAUD CHECK ───────────────────────────────
    Falcon falcon(sess);
    std::string reason;
    if (falcon.checkFraud(accountNumber, amount, reason, FalconChannel::ECOM)) {
      falcon.logFraud(txnId, clientTxnId, "", "", accountNumber, amount,
                      reason);
      return {{"transactionId", txnId},
              {"status", "DECLINED"},
              {"message", reason}};
    }

    // ── PURCHASE ─────────────────────────────────
    if (txnType == "PURCHASE") {

      if (balance < amount)
        return err("ERR_INSUFFICIENT_FUNDS");

      double newBal = balance - amount;

      accounts.update()
          .set("balance", newBal)
          .where("account_number=:a")
          .bind("a", accountNumber)
          .execute();

      auto ins =
          ecomTable
              .insert("transaction_id", "client_txn_id", "account_number",
                      "amount", "currency", "status", "message",
                      "transaction_scope")
              .values(txnId, clientTxnId, accountNumber, amount, currency,
                      "SUCCESS", "ECOM purchase success", scope)
              .execute();

      master.insert("table_name", "reference_id", "status")
          .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
          .execute();

      return {{"transactionId", txnId},
              {"status", "SUCCESS"},
              {"balanceAfter", newBal},
              {"scope", scope}};
    }

    // ── REFUND ───────────────────────────────────
    if (txnType == "REFUND") {

      if (!data.contains("origClientTxnId"))
        return err("ERR_MISSING_ORIG_REF");

      std::string orig = data["origClientTxnId"].get<std::string>();

      auto pRes =
          ecomTable.select("id", "amount")
              .where("client_txn_id=:c AND message='ECOM purchase success'")
              .bind("c", orig)
              .execute();

      if (pRes.count() == 0)
        return err("ERR_ORIGINAL_NOT_FOUND");

      Row pRow = pRes.fetchOne();
      int64_t refId = pRow[0].get<int64_t>();
      double origAmt = pRow[1].get<double>();

      if (amount > origAmt)
        return err("ERR_REFUND_EXCEEDS");

      double newBal = balance + amount;

      accounts.update()
          .set("balance", newBal)
          .where("account_number=:a")
          .bind("a", accountNumber)
          .execute();

      auto ins =
          ecomTable
              .insert("transaction_id", "client_txn_id", "account_number",
                      "amount", "currency", "status", "message", "orig_ref_id")
              .values(txnId, clientTxnId, accountNumber, amount, currency,
                      "SUCCESS", "ECOM refund success", refId)
              .execute();

      master.insert("table_name", "reference_id", "status")
          .values("transaction_ecom", ins.getAutoIncrementValue(), "SUCCESS")
          .execute();

      return {{"transactionId", txnId},
              {"status", "SUCCESS"},
              {"balanceAfter", newBal}};
    }

    return err("ERR_INVALID_TYPE", "Unsupported transactionType");

  } catch (const std::exception &e) {
    return err("ERR_EXCEPTION", e.what());
  }
}

// ═══════════════════════════════════════════════════════
// WRAPPER (TIMEOUT CONTROL)
// ═══════════════════════════════════════════════════════
json processECOMTransaction(const json &data) {

  auto fut = std::async(std::launch::async, processECOMTransactionCore, data);

  if (fut.wait_for(std::chrono::seconds(ECOM_TIMEOUT_SEC)) ==
      std::future_status::timeout) {
    return {{"status", "FAILED"},
            {"errorCode", "ERR_TIMEOUT"},
            {"message", "ECOM transaction timeout"}};
  }

  return fut.get();
}