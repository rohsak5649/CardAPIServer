#include "card_mgmt.h"
#include "Database.h"
#include "TransactionLogger.h"
#include "panencrypted.h"
#include "pin.h"
#include <mysqlx/xdevapi.h>

using namespace mysqlx;
using json = nlohmann::json;

static json cardErr(const std::string& code, const std::string& msg = "") {
    json e{{"errorCode", code}};
    if (!msg.empty()) e["message"] = msg;
    return e;
}

// ── /card/activate ────────────────────────────────────────────────────────────
json processActivateCard(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processActivateCard");
    try {
        std::string encPan = data.value("encryptedPan", "");
        if (encPan.empty()) return cardErr("ERR_MISSING_PAN", "encryptedPan required");

        std::string pan;
        try { pan = PANEncryptionService::getInstance().decryptPAN(encPan); }
        catch (const std::exception& e) { return cardErr("ERR_INVALID_ENCRYPTED_PAN", e.what()); }

        Database::ScopedConnection sc;
        Session& sess = *sc;
        Schema db = sess.getSchema("bankingdb");
        Table cards = db.getTable("cards");

        auto res = cards.select("status", "block_type")
                        .where("pan = :p").bind("p", pan).execute();
        if (res.count() == 0) return cardErr("ERR_CARD_NOT_FOUND");

        Row row = res.fetchOne();
        std::string status = row[0].get<std::string>();
        std::string blockType = row[1].isNull() ? "" : row[1].get<std::string>();

        if (status == "ACTIVE") {
            trace.success({{"reason", "already active"}});
            return {{"status", "SUCCESS"}, {"message", "Card is already ACTIVE"}};
        }
        if (blockType == "PERMANENT") {
            trace.fail("cannot activate permanently blocked card");
            return cardErr("ERR_CARD_PERMANENTLY_BLOCKED", "Card is permanently blocked and cannot be re-activated");
        }

        cards.update()
             .set("status", "ACTIVE")
             .set("block_type", mysqlx::nullvalue)
             .where("pan = :p").bind("p", pan)
             .execute();

        trace.success();
        return {{"status", "SUCCESS"}, {"message", "Card activated successfully"}};

    } catch (const std::exception& e) {
        trace.fail("exception", {{"error", e.what()}});
        return cardErr("ERR_EXCEPTION", e.what());
    }
}

// ── /card/block ───────────────────────────────────────────────────────────────
json processBlockCard(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processBlockCard");
    try {
        std::string encPan  = data.value("encryptedPan", "");
        std::string blockType = data.value("blockType", "TEMPORARY"); // TEMPORARY or PERMANENT
        if (encPan.empty()) return cardErr("ERR_MISSING_PAN", "encryptedPan required");
        if (blockType != "TEMPORARY" && blockType != "PERMANENT")
            return cardErr("ERR_INVALID_BLOCK_TYPE", "blockType must be TEMPORARY or PERMANENT");

        std::string pan;
        try { pan = PANEncryptionService::getInstance().decryptPAN(encPan); }
        catch (const std::exception& e) { return cardErr("ERR_INVALID_ENCRYPTED_PAN", e.what()); }

        Database::ScopedConnection sc;
        Session& sess = *sc;
        Schema db = sess.getSchema("bankingdb");
        Table cards = db.getTable("cards");

        auto res = cards.select("status").where("pan = :p").bind("p", pan).execute();
        if (res.count() == 0) return cardErr("ERR_CARD_NOT_FOUND");

        cards.update()
             .set("status", "BLOCKED")
             .set("block_type", blockType)
             .where("pan = :p").bind("p", pan)
             .execute();

        trace.success({{"blockType", blockType}});
        return {{"status", "SUCCESS"},
                {"message", "Card blocked successfully"},
                {"blockType", blockType}};

    } catch (const std::exception& e) {
        trace.fail("exception", {{"error", e.what()}});
        return cardErr("ERR_EXCEPTION", e.what());
    }
}

// ── /card/set_limit ───────────────────────────────────────────────────────────
json processSetCardLimit(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processSetCardLimit");
    try {
        std::string encPan = data.value("encryptedPan", "");
        if (encPan.empty()) return cardErr("ERR_MISSING_PAN", "encryptedPan required");

        std::string pan;
        try { pan = PANEncryptionService::getInstance().decryptPAN(encPan); }
        catch (const std::exception& e) { return cardErr("ERR_INVALID_ENCRYPTED_PAN", e.what()); }

        Database::ScopedConnection sc;
        Session& sess = *sc;
        Schema db = sess.getSchema("bankingdb");
        Table cards = db.getTable("cards");

        auto res = cards.select("card_id").where("pan = :p").bind("p", pan).execute();
        if (res.count() == 0) return cardErr("ERR_CARD_NOT_FOUND");

        auto update = cards.update();
        bool changed = false;

        if (data.contains("dailyLimit") && data["dailyLimit"].is_number()) {
            double limit = data["dailyLimit"].get<double>();
            if (limit <= 0) return cardErr("ERR_INVALID_LIMIT", "dailyLimit must be > 0");
            update.set("daily_limit", limit);
            changed = true;
        }
        if (data.contains("monthlyLimit") && data["monthlyLimit"].is_number()) {
            double limit = data["monthlyLimit"].get<double>();
            if (limit <= 0) return cardErr("ERR_INVALID_LIMIT", "monthlyLimit must be > 0");
            update.set("monthly_limit", limit);
            changed = true;
        }

        if (!changed) return cardErr("ERR_NOTHING_TO_UPDATE", "Provide dailyLimit and/or monthlyLimit");

        update.where("pan = :p").bind("p", pan).execute();

        trace.success();
        return {{"status", "SUCCESS"},
                {"message", "Card limits updated successfully"},
                {"dailyLimit",   data.value("dailyLimit", nullptr)},
                {"monthlyLimit", data.value("monthlyLimit", nullptr)}};

    } catch (const std::exception& e) {
        trace.fail("exception", {{"error", e.what()}});
        return cardErr("ERR_EXCEPTION", e.what());
    }
}

// ── /card/reset_pin ───────────────────────────────────────────────────────────
json processResetCardPIN(const json& data) {
    TransactionLogger::ScopedFunctionTrace trace("processResetCardPIN");
    try {
        std::string encPan  = data.value("encryptedPan", "");
        std::string newPin  = data.value("newPin", "");
        std::string expiry  = data.value("expiry", "");

        if (encPan.empty()) return cardErr("ERR_MISSING_PAN", "encryptedPan required");
        if (newPin.empty()) return cardErr("ERR_MISSING_PIN", "newPin required");

        std::string pan;
        try { pan = PANEncryptionService::getInstance().decryptPAN(encPan); }
        catch (const std::exception& e) { return cardErr("ERR_INVALID_ENCRYPTED_PAN", e.what()); }

        Database::ScopedConnection sc;
        Session& sess = *sc;
        Schema db = sess.getSchema("bankingdb");
        Table cards = db.getTable("cards");

        auto res = cards.select("status", "expiry").where("pan = :p").bind("p", pan).execute();
        if (res.count() == 0) return cardErr("ERR_CARD_NOT_FOUND");

        Row row = res.fetchOne();
        if (row[0].get<std::string>() == "BLOCKED")
            return cardErr("ERR_CARD_BLOCKED", "Cannot reset PIN of a blocked card");
        if (!expiry.empty() && expiry != row[1].get<std::string>())
            return cardErr("ERR_INVALID_EXPIRY", "Wrong expiry date");

        // Verify the new PIN is what PINService would generate for this PAN
        // (The client should have generated it from the offline tool).
        if (!PINService::getInstance().verifyPIN(pan, newPin))
            return cardErr("ERR_INVALID_PIN", "New PIN does not match PAN-derived PIN");

        // PIN is PAN-derived (HMAC-based) — nothing to store, just confirm.
        trace.success();
        return {{"status", "SUCCESS"},
                {"message", "PIN reset verified — your new PIN is now active"}};

    } catch (const std::exception& e) {
        trace.fail("exception", {{"error", e.what()}});
        return cardErr("ERR_EXCEPTION", e.what());
    }
}
