
/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* CARD ISSUANCE PROCESSING FLOW:
*
* 1. REQUEST HANDLING:
*    - Card issuance request received from client/API.
*    - Required fields:
*        → accountNumber
*        → cardholderName
*        → scheme (VISA / MASTERCARD / RUPAY)
*
* 2. INPUT VALIDATION:
*    - Validate presence of mandatory fields.
*    - Normalize scheme to uppercase.
*    - Validate supported schemes:
*        → VISA
*        → MASTERCARD
*        → RUPAY
*    - If invalid → request rejected.
*
* 3. ACCOUNT VALIDATION:
*    - Verify account existence in accounts table.
*    - If account not found → transaction rejected.
*
* 4. CARD PRIORITY ASSIGNMENT:
*    - Count existing cards linked to account.
*    - Assign priority based on count:
*        → 0 cards → PRIMARY
*        → 1 card  → SECONDARY
*        → 2 cards → TERTIARY
*    - Max limit: 3 cards per account.
*    - If exceeded → request rejected.
*
* 5. CARD DATA GENERATION:
*
*    BIN GENERATION:
*    - VISA        → 411111
*    - MASTERCARD  → 550000
*    - RUPAY       → 608014
*
*    PAN GENERATION:
*    - BIN + random digits (16-digit PAN)
*    - Generated using secure random generator.
*
*    CVV GENERATION:
*    - 3-digit random number (100–999)
*
*    EXPIRY GENERATION:
*    - Currently static (demo: 12/26)
*    - Can be enhanced to dynamic future date.
*
*    CARD TYPE:
*    - RUPAY → DOMESTIC
*    - Others → INTERNATIONAL
*
* 6. DATABASE INSERTION:
*    - Insert new record into cards table with:
*        → PAN
*        → Scheme
*        → Card type
*        → Expiry
*        → CVV
*        → Cardholder name
*        → Account number
*        → Status = ACTIVE
*        → Priority (PRIMARY/SECONDARY/TERTIARY)
*
* 7. RESPONSE FORMATION:
*    - PAN is masked before returning:
*        → First 6 digits visible
*        → Remaining digits masked
*
*    - Response includes:
*        → masked PAN
*        → scheme
*        → card type
*        → expiry
*        → CVV (⚠ demo only)
*        → cardholder name
*        → account number
*
* 8. ERROR HANDLING:
*    - ERR_INVALID_REQUEST → missing fields
*    - ERR_INVALID_SCHEME → unsupported scheme
*    - ERR_ACCOUNT_NOT_FOUND → invalid account
*    - ERR_MAX_CARD_LIMIT → more than 3 cards
*    - ERR_DB → database errors
*
* SECURITY NOTES:
* - PAN is generated securely using randomization.
* - PAN should be encrypted at rest in production.
* - CVV must NEVER be stored or returned in real systems.
* - Sensitive data exposure here is for demo purposes only.
*
* DESIGN NOTES:
* - Modular helper functions used:
*        → getBIN()
*        → generatePAN()
*        → generateCVV()
*        → generateExpiry()
*        → maskPAN()
*
* - Uses centralized Database session for DB operations.
*
* FUTURE ENHANCEMENTS:
* - Luhn algorithm validation for PAN
* - Dynamic expiry generation
* - CVV hashing or elimination
* - PAN encryption (PCI-DSS compliance)
* - Card activation workflow (OTP based)
*
* Unauthorized modification without understanding card generation
* and security implications is strongly discouraged.
*
* For implementation details, contact: +91 9112765649
*/
#include <iostream>
#include "json.hpp"
#include <mysqlx/xdevapi.h>
#include <random>
#include <chrono>
#include <sstream>
#include "Database.h"

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
        Session& sess = Database::getSession();
        Schema db = Database::getSchema();

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
            //sess.close();
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
            //sess.close();
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
