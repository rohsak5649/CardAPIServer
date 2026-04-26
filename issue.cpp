/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * CARD ISSUANCE PROCESSING  v3.0  (C++20 · Thread-Safe)
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ Luhn algorithm added — generated PAN passes Luhn check
 *   ✅ std::string_view params, [[nodiscard]]
 *   ✅ Expiry date generated dynamically (5 years from issue)
 *   ✅ constexpr MAX_CARDS_PER_ACCOUNT
 *   ✅ Scheme-to-BIN map uses std::unordered_map (OCP — add new schemes easily)
 *   ✅ ScopedConnection RAII — no raw Session
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "issue.h"
#include "Database.h"

#include <iostream>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <mysqlx/xdevapi.h>

using namespace mysqlx;
using json = nlohmann::json;

inline constexpr int MAX_CARDS_PER_ACCOUNT = 3;

// OCP: add new schemes here without touching any logic below
static const std::unordered_map<std::string,std::string> SCHEME_TO_BIN = {
    { "VISA",       "411111" },
    { "MASTERCARD", "550000" },
    { "RUPAY",      "608014" },
    { "AMEX",       "371449" },  // easily extensible
};

// ── Luhn check digit ──────────────────────────────────────────────────────────
[[nodiscard]] static char luhnDigit(std::string_view partial) {
    // partial = first 15 digits (no check digit yet)
    int sum = 0;
    bool alt = true;
    for (int i = static_cast<int>(partial.size())-1; i >= 0; --i) {
        int d = partial[i] - '0';
        if (alt) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
        alt = !alt;
    }
    int check = (10 - (sum % 10)) % 10;
    return static_cast<char>('0' + check);
}

[[nodiscard]] static std::string generatePAN(std::string_view bin) {
    std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<> d(0,9);

    // Generate 9 random digits after the 6-digit BIN (total 15 before Luhn)
    std::string pan{bin};
    for (int i = 0; i < 9; ++i) pan += static_cast<char>('0' + d(gen));
    pan += luhnDigit(pan);  // 16th digit
    return pan;
}

[[nodiscard]] static std::string generateCVV() {
    std::mt19937 gen{std::random_device{}()};
    std::ostringstream oss;
    oss << std::setw(3) << std::setfill('0')
        << std::uniform_int_distribution<>(100,999)(gen);
    return oss.str();
}

[[nodiscard]] static std::string expiryDate() {
    // 5 years from today, MM/YY
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_p = std::localtime(&t);
    int yr  = (tm_p->tm_year + 1900 + 5) % 100;  // last 2 digits, +5 years
    int mon = tm_p->tm_mon + 1;
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << mon << "/"
        << std::setw(2) << std::setfill('0') << yr;
    return oss.str();
}

[[nodiscard]] static std::string maskPAN(std::string_view pan) {
    if (pan.size() < 6) return "XXXXXX********";
    return std::string(pan.substr(0,6)) + "XXXXXXXXXX";
}

// ── Public entry point ────────────────────────────────────────────────────────
json processIssueCard(const json& data) {
    json response;
    Database::ScopedConnection sc;
    Session& sess = *sc;
    Schema   db   = sess.getSchema("bankingdb");

    try {
        if (!data.contains("accountNumber") ||
            !data.contains("cardholderName") ||
            !data.contains("scheme")) {
            response["errorCode"] = "ERR_INVALID_REQUEST";
            response["message"]   = "Missing: accountNumber, cardholderName, scheme";
            return response;
        }

        std::string account = data["accountNumber"].get<std::string>();
        std::string name    = data["cardholderName"].get<std::string>();
        std::string scheme  = data["scheme"].get<std::string>();
        std::transform(scheme.begin(),scheme.end(),scheme.begin(),::toupper);

        auto binIt = SCHEME_TO_BIN.find(scheme);
        if (binIt == SCHEME_TO_BIN.end()) {
            response["errorCode"] = "ERR_INVALID_SCHEME";
            response["message"]   = "Allowed: VISA, MASTERCARD, RUPAY, AMEX";
            return response;
        }

        std::string cardType = (scheme == "RUPAY") ? "DOMESTIC" : "INTERNATIONAL";

        Table accounts = db.getTable("accounts");
        Table cards    = db.getTable("cards");

        auto accRes = accounts.select("account_id")
            .where("account_number=:acc").bind("acc",account).execute();
        if (accRes.count() == 0) {
            response["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            response["message"]   = "Account does not exist";
            return response;
        }

        int cardCount = cards.select("COUNT(*)")
            .where("account_number=:acc").bind("acc",account).execute()
            .fetchOne()[0].get<int>();

        if (cardCount >= MAX_CARDS_PER_ACCOUNT) {
            response["errorCode"] = "ERR_MAX_CARD_LIMIT";
            response["message"]   = "Maximum " + std::to_string(MAX_CARDS_PER_ACCOUNT) +
                                    " cards allowed per account";
            return response;
        }

        static constexpr const char* PRIORITY[] = { "PRIMARY","SECONDARY","TERTIARY" };
        std::string priority = PRIORITY[cardCount];

        std::string pan    = generatePAN(binIt->second);
        std::string expiry = expiryDate();
        std::string cvv    = generateCVV();

        cards.insert("pan","scheme","card_type","expiry","cvv",
                     "cardholder_name","account_number","status","card_priority")
             .values(pan,scheme,cardType,expiry,cvv,name,account,"ACTIVE",priority)
             .execute();

        response["status"]           = "SUCCESS";
        response["message"]          = "Card issued successfully";
        response["priorityAssigned"] = priority;
        response["card"] = {
            {"pan",           maskPAN(pan)},
            {"scheme",        scheme},
            {"cardType",      cardType},
            {"expiry",        expiry},
            {"cvv",           cvv},
            {"cardholderName",name},
            {"accountNumber", account}
        };

    } catch (const mysqlx::Error& er) {
        response["errorCode"] = "ERR_DB";
        response["message"]   = er.what();
    } catch (const std::exception& ex) {
        response["errorCode"] = "ERR_UNKNOWN";
        response["message"]   = ex.what();
    }
    return response;
}