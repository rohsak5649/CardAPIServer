/* Copyright (c) Rohan Sakhare
 * All rights reserved.
 *
 * Card Issuance Flow:
 * 1. Client submits account number, cardholder name, and scheme.
 * 2. System validates request fields and supported card schemes.
 * 3. Account existence verified in database.
 * 4. Card priority assigned (Primary / Secondary / Tertiary).
 * 5. Unique PAN, CVV, and Expiry generated securely.
 * 6. Card details stored in Cards table with ACTIVE status.
 * 7. Masked PAN returned in response (CVV shown for demo only).
 *
 * Unauthorized copying or modification without understanding
 * the card issuance logic is discouraged.
 *
 * For implementation details, contact: +91 9112765649
 */
#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <random>
#include <chrono>
#include <sstream>

using namespace mysqlx;
using json = nlohmann::json;
// ---------------------- BIN DEFINITIONS ------------------------
std::string getBIN(const std::string& scheme) {

    if (scheme == "VISA")
        return "411111";

    if (scheme == "MASTERCARD")
        return "550000";

    if (scheme == "RUPAY")
        return "608014";

    throw std::invalid_argument("Invalid card scheme");
}

// ---------------------- GENERATE PAN ---------------------------
std::string generatePAN(const std::string& bin) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 9);

    std::string pan = bin;
    for (int i = 0; i < 10; i++)
        pan += std::to_string(dist(gen));

    return pan;
}

// ---------------------- GENERATE CVV ---------------------------
std::string generateCVV() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(100, 999);
    return std::to_string(dist(gen));
}

// ---------------------- GENERATE EXPIRY ------------------------
std::string generateExpiry() {
    return "12/26";  // demo static
}

// ---------------------- MASK PAN ------------------------
std::string maskPAN(const std::string& pan) {
    if (pan.length() < 6)
        return "XXXXXX********";

    return pan.substr(0, 6) + "XXXXXXXXXX";
}

// ---------------------- MAIN API: ISSUE CARD ------------------
json processIssueCard(const json& data) {

    json response;

    try {

        if (!data.contains("accountNumber") ||
            !data.contains("cardholderName") ||
            !data.contains("scheme")) {

            response["errorCode"] = "ERR_INVALID_REQUEST";
            response["message"] = "Missing required fields.";
            return response;
        }

        std::string account = data["accountNumber"];
        std::string name = data["cardholderName"];
        std::string scheme = data["scheme"];

        // ----------- Make Scheme Case-Insensitive -------------
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::toupper);

        // ----------- STRICT SCHEME VALIDATION ------------------
        if (scheme != "VISA" &&
            scheme != "MASTERCARD" &&
            scheme != "RUPAY") {

            response["errorCode"] = "ERR_INVALID_SCHEME";
            response["message"] =
                "Unsupported card scheme. Allowed values: VISA, MASTERCARD, RUPAY.";
            return response;
        }

        std::string cardType =
            (scheme == "RUPAY") ? "DOMESTIC" : "INTERNATIONAL";

        // ---------------- DB CONNECTION ------------------------
        Session sess("localhost", 33060, "root", "YourPassword");
        Schema db = sess.getSchema("bankingdb");

        Table accounts = db.getTable("accounts");
        Table cards = db.getTable("cards");

        // ----------- Validate Account --------------------------
        RowResult accRes = accounts.select("account_id")
            .where("account_number = :acc")
            .bind("acc", account)
            .execute();

        if (accRes.count() == 0) {
            response["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
            response["message"] = "Account does not exist.";
            sess.close();
            return response;
        }

        // ----------- Check Existing Cards ----------------------
        RowResult cardCountRes = cards.select("COUNT(*)")
            .where("account_number = :acc")
            .bind("acc", account)
            .execute();

        int cardCount = cardCountRes.fetchOne()[0].get<int>();

        std::string priority;

        if (cardCount == 0)
            priority = "PRIMARY";
        else if (cardCount == 1)
            priority = "SECONDARY";
        else if (cardCount == 2)
            priority = "TERTIARY";
        else {
            response["errorCode"] = "ERR_MAX_CARD_LIMIT";
            response["message"] =
                "Maximum number of cards reached (3).";
            sess.close();
            return response;
        }

        // ----------- Generate Card Data ------------------------
        std::string bin = getBIN(scheme);
        std::string pan = generatePAN(bin);
        std::string expiry = generateExpiry();
        std::string cvv = generateCVV();

        // ----------- Insert into DB ----------------------------
        cards.insert(
            "pan", "scheme", "card_type", "expiry",
            "cvv", "cardholder_name", "account_number",
            "status", "card_priority"
        )
        .values(
            pan, scheme, cardType, expiry,
            cvv, name, account,
            "ACTIVE", priority
        )
        .execute();

        sess.close();

        // ----------- Mask PAN for Response ---------------------
        std::string maskedPAN = maskPAN(pan);

        // ----------- Success Response --------------------------
        response["status"] = "SUCCESS";
        response["message"] = "Card issued successfully";
        response["priorityAssigned"] = priority;

        response["card"] = {
            {"pan", maskedPAN},
            {"scheme", scheme},
            {"cardType", cardType},
            {"expiry", expiry},
            {"cvv", cvv},   // ⚠ In real banking NEVER return CVV
            {"cardholderName", name},
            {"accountNumber", account}
        };
    }
    catch (const mysqlx::Error& err) {
        response["errorCode"] = "ERR_DB";
        response["message"] = err.what();
    }
    catch (const std::exception& ex) {
        response["errorCode"] = "ERR_UNKNOWN";
        response["message"] = ex.what();
    }

    return response;
}
