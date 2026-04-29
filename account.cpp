#include "account.h"
#include "AccountLockManager.h"
#include "Database.h"
#include "TransactionLogger.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mysqlx/xdevapi.h>
#include <optional>
#include <string>

using namespace mysqlx;
using json = nlohmann::json;

namespace {

struct CurrencyInfo {
  int id;
  std::string code;
  std::string name;
};

[[nodiscard]] json err(const std::string &code, const std::string &message) {
  return {{"errorCode", code}, {"message", message}};
}

[[nodiscard]] std::string trim(std::string value) {
  auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
              value.end());
  return value;
}

[[nodiscard]] std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

[[nodiscard]] bool validCountryCode(const std::string &countryCode) {
  return countryCode.size() == 2 &&
         std::all_of(countryCode.begin(), countryCode.end(), [](unsigned char ch) {
           return std::isalpha(ch);
         });
}

[[nodiscard]] std::optional<std::string>
readAccountNumber(const json &data, json &errorResponse) {
  TransactionLogger::ScopedFunctionTrace trace("readAccountNumber");
  if (!data.contains("accountNumber") || !data["accountNumber"].is_string()) {
    errorResponse =
        err("ERR_INVALID_REQUEST", "Missing required field: accountNumber");
    trace.fail("missing required field: accountNumber");
    return std::nullopt;
  }

  std::string accountNumber = trim(data["accountNumber"].get<std::string>());
  if (accountNumber.empty() || accountNumber.size() > 20) {
    errorResponse = err("ERR_INVALID_ACCOUNT_NUMBER",
                        "accountNumber must be 1 to 20 characters");
    trace.fail("invalid account number length");
    return std::nullopt;
  }
  trace.success({{"accountNumber", accountNumber}});
  return accountNumber;
}

[[nodiscard]] std::optional<CurrencyInfo>
resolveCurrency(Session &sess, const json &data, json &errorResponse) {
  TransactionLogger::ScopedFunctionTrace trace("resolveCurrency");
  if (data.contains("currencyId")) {
    if (!data["currencyId"].is_number_integer() &&
        !data["currencyId"].is_number_unsigned()) {
      errorResponse =
          err("ERR_INVALID_CURRENCY", "currencyId must be a positive integer");
      trace.fail("currencyId is not an integer");
      return std::nullopt;
    }

    int currencyId = 0;
    if (data["currencyId"].is_number_unsigned()) {
      auto rawId = data["currencyId"].get<unsigned long long>();
      if (rawId > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
        errorResponse =
            err("ERR_INVALID_CURRENCY", "currencyId must fit in a positive integer");
        trace.fail("currencyId exceeds integer range");
        return std::nullopt;
      }
      currencyId = static_cast<int>(rawId);
    } else {
      currencyId = data["currencyId"].get<int>();
    }

    if (currencyId <= 0) {
      errorResponse =
          err("ERR_INVALID_CURRENCY", "currencyId must be a positive integer");
      trace.fail("currencyId is not positive");
      return std::nullopt;
    }

    auto res = sess
                   .sql("SELECT currency_id, currency_code, currency_name "
                        "FROM currency WHERE currency_id=?")
                   .bind(currencyId)
                   .execute();
    if (res.count() == 0) {
      errorResponse =
          err("ERR_CURRENCY_NOT_FOUND", "currencyId does not exist");
      trace.fail("currencyId does not exist", {{"currencyId", std::to_string(currencyId)}});
      return std::nullopt;
    }

    Row row = res.fetchOne();
    CurrencyInfo currency{row[0].get<int>(), row[1].get<std::string>(),
                          row[2].get<std::string>()};
    trace.success({{"currencyId", std::to_string(currency.id)},
                   {"currencyCode", currency.code}});
    return currency;
  }

  if (data.contains("currencyCode")) {
    if (!data["currencyCode"].is_string()) {
      errorResponse =
          err("ERR_INVALID_CURRENCY", "currencyCode must be a string");
      trace.fail("currencyCode is not a string");
      return std::nullopt;
    }

    std::string currencyCode =
        upper(trim(data["currencyCode"].get<std::string>()));
    if (currencyCode.size() != 3) {
      errorResponse =
          err("ERR_INVALID_CURRENCY", "currencyCode must be a 3-letter code");
      trace.fail("currencyCode is invalid", {{"currencyCode", currencyCode}});
      return std::nullopt;
    }

    auto res = sess
                   .sql("SELECT currency_id, currency_code, currency_name "
                        "FROM currency WHERE currency_code=?")
                   .bind(currencyCode)
                   .execute();
    if (res.count() == 0) {
      errorResponse =
          err("ERR_CURRENCY_NOT_FOUND", "currencyCode does not exist");
      trace.fail("currencyCode does not exist", {{"currencyCode", currencyCode}});
      return std::nullopt;
    }

    Row row = res.fetchOne();
    CurrencyInfo currency{row[0].get<int>(), row[1].get<std::string>(),
                          row[2].get<std::string>()};
    trace.success({{"currencyId", std::to_string(currency.id)},
                   {"currencyCode", currency.code}});
    return currency;
  }

  errorResponse =
      err("ERR_INVALID_REQUEST", "Missing required field: currencyId or currencyCode");
  trace.fail("missing currencyId or currencyCode");
  return std::nullopt;
}

[[nodiscard]] json accountFromRow(const Row &row) {
  return {{"accountId", row[0].get<int>()},
          {"accountNumber", row[1].get<std::string>()},
          {"balance", row[2].get<double>()},
          {"countryCode", row[3].get<std::string>()},
          {"isFrozen", row[4].get<int>() != 0},
          {"currency",
           {{"currencyId", row[5].get<int>()},
            {"currencyCode", row[6].get<std::string>()},
            {"currencyName", row[7].get<std::string>()}}}};
}

[[nodiscard]] std::string accountSelectSql(const std::string &whereClause) {
  return "SELECT a.account_id, a.account_number, a.balance, a.country_code, "
         "a.is_frozen, c.currency_id, c.currency_code, c.currency_name "
         "FROM accounts a JOIN currency c ON c.currency_id = a.currency_id " +
         whereClause;
}

[[nodiscard]] json fetchAccountByNumber(Session &sess,
                                        const std::string &accountNumber) {
  TransactionLogger::ScopedFunctionTrace trace("fetchAccountByNumber",
                                               {{"accountNumber", accountNumber}});
  auto result = sess.sql(accountSelectSql("WHERE a.account_number=?"))
                    .bind(accountNumber)
                    .execute();
  if (result.count() == 0) {
    trace.fail("account not found", {{"accountNumber", accountNumber}});
    return err("ERR_ACCOUNT_NOT_FOUND", "Account number does not exist");
  }

  json response;
  response["status"] = "SUCCESS";
  response["account"] = accountFromRow(result.fetchOne());
  trace.success({{"status", "SUCCESS"}});
  return response;
}

[[nodiscard]] json setFreezeStatus(const json &data, bool frozen) {
  TransactionLogger::ScopedFunctionTrace trace("setFreezeStatus",
                                               {{"targetFrozen", frozen ? "true" : "false"}});
  try {
    json accountError;
    std::optional<std::string> accountNumber =
        readAccountNumber(data, accountError);
    if (!accountNumber) {
      trace.fail("invalid account number for freeze operation");
      return accountError;
    }

    AccountLockManager::ScopedLock accountLock(
        AccountLockManager::getInstance(), *accountNumber, TxnPriority::CREDIT);

    Database::ScopedConnection sc;
    Session &sess = *sc;

    try {
      sess.startTransaction();

      auto exists = sess
                        .sql("SELECT account_id FROM accounts "
                             "WHERE account_number=? FOR UPDATE")
                        .bind(*accountNumber)
                        .execute();
      if (exists.count() == 0) {
        sess.rollback();
        trace.fail("account not found during freeze update",
                   {{"accountNumber", *accountNumber}});
        return err("ERR_ACCOUNT_NOT_FOUND", "Account number does not exist");
      }

      sess.sql("UPDATE accounts SET is_frozen=? WHERE account_number=?")
          .bind(frozen ? 1 : 0, *accountNumber)
          .execute();
      sess.commit();
    } catch (...) {
      try {
        sess.rollback();
      } catch (...) {
      }
      throw;
    }

    json response = fetchAccountByNumber(sess, *accountNumber);
    response["message"] =
        frozen ? "Account frozen successfully" : "Account unfrozen successfully";
    trace.success({{"accountNumber", *accountNumber},
                   {"isFrozen", frozen ? "true" : "false"}});
    return response;

  } catch (const mysqlx::Error &er) {
    trace.fail("database error while changing freeze status", {{"error", er.what()}});
    return err("ERR_DB", er.what());
  } catch (const std::exception &ex) {
    trace.fail("exception while changing freeze status", {{"error", ex.what()}});
    return err("ERR_EXCEPTION", ex.what());
  }
}

} // namespace

json processAddAccount(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processAddAccount");
  try {
    json accountError;
    std::optional<std::string> accountNumber =
        readAccountNumber(data, accountError);
    if (!accountNumber) {
      trace.fail("invalid account number");
      return accountError;
    }

    double balance = 0.0;
    if (data.contains("balance")) {
      if (!data["balance"].is_number()) {
        trace.fail("balance is not numeric");
        return err("ERR_INVALID_BALANCE", "balance must be a number");
      }
      balance = data["balance"].get<double>();
    } else if (data.contains("initialBalance")) {
      if (!data["initialBalance"].is_number()) {
        trace.fail("initialBalance is not numeric");
        return err("ERR_INVALID_BALANCE", "initialBalance must be a number");
      }
      balance = data["initialBalance"].get<double>();
    }

    if (balance < 0.0) {
      trace.fail("negative balance supplied", {{"balance", std::to_string(balance)}});
      return err("ERR_INVALID_BALANCE", "balance cannot be negative");
    }

    std::string countryCode = "AU";
    if (data.contains("countryCode")) {
      if (!data["countryCode"].is_string()) {
        trace.fail("countryCode is not a string");
        return err("ERR_INVALID_COUNTRY_CODE", "countryCode must be a string");
      }
      countryCode = upper(trim(data["countryCode"].get<std::string>()));
    }
    if (!validCountryCode(countryCode)) {
      trace.fail("invalid countryCode", {{"countryCode", countryCode}});
      return err("ERR_INVALID_COUNTRY_CODE",
                 "countryCode must be a 2-letter country code");
    }

    bool isFrozen = false;
    if (data.contains("isFrozen")) {
      if (!data["isFrozen"].is_boolean()) {
        trace.fail("isFrozen is not boolean");
        return err("ERR_INVALID_REQUEST", "isFrozen must be true or false");
      }
      isFrozen = data["isFrozen"].get<bool>();
    }

    Database::ScopedConnection sc;
    Session &sess = *sc;

    json currencyError;
    std::optional<CurrencyInfo> currency =
        resolveCurrency(sess, data, currencyError);
    if (!currency) {
      trace.fail("currency resolution failed");
      return currencyError;
    }

    auto existing = sess
                        .sql("SELECT account_id FROM accounts "
                             "WHERE account_number=?")
                        .bind(*accountNumber)
                        .execute();
    if (existing.count() > 0) {
      trace.fail("account already exists", {{"accountNumber", *accountNumber}});
      return err("ERR_ACCOUNT_EXISTS", "Account number already exists");
    }

    auto inserted =
        sess
            .sql("INSERT INTO accounts "
                 "(account_number, balance, country_code, is_frozen, currency_id) "
                 "VALUES (?, ?, ?, ?, ?)")
            .bind(*accountNumber, balance, countryCode, isFrozen ? 1 : 0,
                  currency->id)
            .execute();

    json response;
    response["status"] = "SUCCESS";
    response["message"] = "Account added successfully";
    response["account"] = {
        {"accountId", inserted.getAutoIncrementValue()},
        {"accountNumber", *accountNumber},
        {"balance", balance},
        {"countryCode", countryCode},
        {"isFrozen", isFrozen},
        {"currency",
         {{"currencyId", currency->id},
          {"currencyCode", currency->code},
          {"currencyName", currency->name}}}};
    trace.success({{"accountNumber", *accountNumber},
                   {"accountId", std::to_string(inserted.getAutoIncrementValue())},
                   {"currencyCode", currency->code}});
    return response;

  } catch (const mysqlx::Error &er) {
    std::string message = er.what();
    if (message.find("Duplicate") != std::string::npos ||
        message.find("duplicate") != std::string::npos) {
      trace.fail("duplicate account database error", {{"error", message}});
      return err("ERR_ACCOUNT_EXISTS", "Account number already exists");
    }
    trace.fail("database error while adding account", {{"error", message}});
    return err("ERR_DB", message);
  } catch (const std::exception &ex) {
    trace.fail("exception while adding account", {{"error", ex.what()}});
    return err("ERR_EXCEPTION", ex.what());
  }
}

json processGetAccount(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processGetAccount");
  try {
    json accountError;
    std::optional<std::string> accountNumber =
        readAccountNumber(data, accountError);
    if (!accountNumber) {
      trace.fail("invalid account number");
      return accountError;
    }

    Database::ScopedConnection sc;
    json response = fetchAccountByNumber(*sc, *accountNumber);
    if (response.contains("errorCode")) {
      trace.fail("account lookup failed",
                 {{"accountNumber", *accountNumber},
                  {"errorCode", response["errorCode"].get<std::string>()}});
    } else {
      trace.success({{"accountNumber", *accountNumber}});
    }
    return response;

  } catch (const mysqlx::Error &er) {
    trace.fail("database error while getting account", {{"error", er.what()}});
    return err("ERR_DB", er.what());
  } catch (const std::exception &ex) {
    trace.fail("exception while getting account", {{"error", ex.what()}});
    return err("ERR_EXCEPTION", ex.what());
  }
}

json processFreezeAccount(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processFreezeAccount");
  json response = setFreezeStatus(data, true);
  if (response.contains("errorCode")) {
    trace.fail("freeze account failed",
               {{"errorCode", response["errorCode"].get<std::string>()}});
  } else {
    trace.success();
  }
  return response;
}

json processUnfreezeAccount(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processUnfreezeAccount");
  json response = setFreezeStatus(data, false);
  if (response.contains("errorCode")) {
    trace.fail("unfreeze account failed",
               {{"errorCode", response["errorCode"].get<std::string>()}});
  } else {
    trace.success();
  }
  return response;
}

json processListAccounts(const json &data) {
  TransactionLogger::ScopedFunctionTrace trace("processListAccounts");
  try {
    int limit = 50;
    int offset = 0;
    if (data.contains("limit")) {
      if (!data["limit"].is_number_integer()) {
        trace.fail("limit is not integer");
        return err("ERR_INVALID_REQUEST", "limit must be an integer");
      }
      limit = data["limit"].get<int>();
    }
    if (data.contains("offset")) {
      if (!data["offset"].is_number_integer()) {
        trace.fail("offset is not integer");
        return err("ERR_INVALID_REQUEST", "offset must be an integer");
      }
      offset = data["offset"].get<int>();
    }

    if (limit < 1 || limit > 100) {
      trace.fail("limit outside allowed range", {{"limit", std::to_string(limit)}});
      return err("ERR_INVALID_REQUEST", "limit must be between 1 and 100");
    }
    if (offset < 0) {
      trace.fail("offset is negative", {{"offset", std::to_string(offset)}});
      return err("ERR_INVALID_REQUEST", "offset cannot be negative");
    }

    Database::ScopedConnection sc;
    Session &sess = *sc;
    auto result = sess
                      .sql(accountSelectSql(
                               "ORDER BY a.account_id DESC LIMIT ? OFFSET ?"))
                      .bind(limit, offset)
                      .execute();

    json accounts = json::array();
    while (Row row = result.fetchOne()) {
      accounts.push_back(accountFromRow(row));
    }

    trace.success({{"limit", std::to_string(limit)},
                   {"offset", std::to_string(offset)},
                   {"count", std::to_string(accounts.size())}});
    return {{"status", "SUCCESS"},
            {"limit", limit},
            {"offset", offset},
            {"count", accounts.size()},
            {"accounts", accounts}};

  } catch (const mysqlx::Error &er) {
    trace.fail("database error while listing accounts", {{"error", er.what()}});
    return err("ERR_DB", er.what());
  } catch (const std::exception &ex) {
    trace.fail("exception while listing accounts", {{"error", ex.what()}});
    return err("ERR_EXCEPTION", ex.what());
  }
}
