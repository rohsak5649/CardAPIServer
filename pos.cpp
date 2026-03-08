/*
* POS Transaction Processing Flow:
 *
 * PURCHASE:
 * 1. POS sends card and payment details.
 * 2. System validates card status, CVV, and linked account.
 * 3. Balance check performed (amount + fee).
 * 4. Amount debited atomically from customer account.
 * 5. POS transaction stored and linked in master records.
 * 6. Success response returned with updated balance.
 *
 * REFUND:
 * 1. POS sends original transaction reference.
 * 2. System validates original purchase and merchant/terminal.
 * 3. Ensures refund does not exceed original purchase amount.
 * 4. Amount credited atomically back to customer account.
 * 5. Refund record stored with linkage to original purchase.
 * 6. Response returned with refund status and balance update.
 *
 * Ensures transactional integrity using database transactions.
 */

#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <mysqlx/xdevapi.h>
#include "json.hpp"

using namespace mysqlx;
using json = nlohmann::json;

static std::string genPosTxnId() {
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int r = std::rand() % 9000 + 1000;
    std::ostringstream oss;
    oss << "pos-txn-" << ms << "-" << r;
    return oss.str();
}

json processPOSTransaction(const json &data) {
    json res;

    // We’ll keep one session for the whole function
    // and use transactions only when we modify balances.
    try {
        // --------------- Common fields ----------------
        std::string clientTxnId = data.value("clientTxnId", genPosTxnId());
        std::string txnType     = data.value("transactionType", std::string("PURCHASE"));
        std::string merchantId  = data.value("merchantId", std::string());
        std::string terminalId  = data.value("terminalId", std::string());
        std::string location    = data.value("location", std::string());
        double amount           = data.value("amount", 0.0);
        double fee              = data.value("fee", 0.0);
        std::string currency    = data.value("currency", std::string("USD"));

        if (amount < 0.0) {
            res["errorCode"] = "ERR_INVALID_AMOUNT";
            res["message"]   = "Amount cannot be negative.";
            return res;
        }

        // DB connection (adjust if needed)
        Session sess("localhost", 33060, "root", "Rohan@5649");
        Schema db = sess.getSchema("bankingdb");
        Table cards    = db.getTable("cards");
        Table accounts = db.getTable("accounts");
        Table posTbl   = db.getTable("transaction_pos");
        Table master   = db.getTable("transactions");

        bool txStarted = false;
        std::string txnId = genPosTxnId();

        // ==========================================================
        //                        PURCHASE
        // ==========================================================
        if (txnType == "PURCHASE") {
            if (!data.contains("card")) {
                res["errorCode"] = "ERR_MISSING_CARD";
                res["message"]   = "card object required for purchase.";
                return res;
            }

            json card = data["card"];
            std::string pan    = card.value("pan", std::string());
            std::string expiry = card.value("expiry", std::string());
            std::string cvv    = card.value("cvv", std::string());

            if (pan.empty() || expiry.empty()) {
                res["errorCode"] = "ERR_CARD_INCOMPLETE";
                res["message"]   = "PAN and expiry required.";
                return res;
            }

            // Validate card from cards table
            RowResult cardRes = cards.select("account_number","scheme","cvv","status")
                .where("pan = :p AND expiry = :e")
                .bind("p", pan)
                .bind("e", expiry)
                .execute();

            if (cardRes.count() == 0) {
                res["errorCode"] = "ERR_CARD_NOT_FOUND";
                res["message"]   = "Card not found for given PAN/expiry.";
                return res;
            }

            Row cardRow = cardRes.fetchOne();
            std::string accNo      = cardRow[0].get<std::string>();
            std::string cardScheme = cardRow[1].isNull()? std::string() : cardRow[1].get<std::string>();
            std::string dbCvv      = cardRow[2].isNull()? std::string() : cardRow[2].get<std::string>();
            std::string cardStatus = cardRow[3].isNull()? std::string() : cardRow[3].get<std::string>();

            if (cardStatus != "ACTIVE") {
                res["errorCode"] = "ERR_CARD_NOT_ACTIVE";
                res["message"]   = "Card is not active.";
                return res;
            }

            // Optional CVV check
            if (!cvv.empty() && !dbCvv.empty() && cvv != dbCvv) {
                res["errorCode"] = "ERR_CVV_MISMATCH";
                res["message"]   = "Provided CVV does not match stored CVV.";
                return res;
            }

            // Load account
            RowResult accRes = accounts.select("currency","balance")
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            if (accRes.count() == 0) {
                res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
                res["message"]   = "Linked account not found.";
                return res;
            }

            Row accRow = accRes.fetchOne();
            std::string accCurrency = accRow[0].get<std::string>();
            double balance          = accRow[1].get<double>();

            std::string scope = (accCurrency == currency) ? "DOMESTIC" : "INTERNATIONAL";

            double totalDebit = amount + fee;
            if (totalDebit <= 0.0) {
                res["errorCode"] = "ERR_INVALID_AMOUNT";
                res["message"]   = "Total debit must be positive.";
                return res;
            }

            if (balance < totalDebit) {
                res["errorCode"] = "ERR_INSUFFICIENT_FUNDS";
                res["message"]   = "Not enough balance.";
                return res;
            }

            // ---- Start atomic DB transaction for debit + inserts ----
            txStarted = true;
            sess.startTransaction();

            double newBal = balance - totalDebit;

            // Update balance
            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            // Insert purchase row (original_purchase_id is NULL)
            auto ins = posTbl.insert(
                "transaction_id","client_txn_id","merchant_id","terminal_id",
                "location","account_number","amount","fee","card_pan","card_scheme",
                "status","message","original_purchase_id"
            ).values(
                txnId, clientTxnId, merchantId, terminalId,
                location, accNo, amount, fee, pan, cardScheme,
                "SUCCESS", "POS purchase successful", nullptr
            ).execute();

            int64_t childId = ins.getAutoIncrementValue();

            // Insert into master table
            master.insert("table_name","reference_id","status")
                .values("transaction_pos", childId, "SUCCESS")
                .execute();

            sess.commit();
            txStarted = false;

            res["transactionId"]    = txnId;
            res["status"]           = "SUCCESS";
            res["balanceAfter"]     = newBal;
            res["transactionScope"] = scope;
            return res;
        }

        // ==========================================================
        //                          REFUND
        // ==========================================================
        if (txnType == "REFUND") {
            // Prefer origTransactionId (transaction_pos.transaction_id)
            std::string origTransId  = data.value("origTransactionId", std::string());
            std::string origClientId = data.value("origClientTxnId", std::string());

            if (origTransId.empty() && origClientId.empty()) {
                res["errorCode"] = "ERR_MISSING_ORIGINAL";
                res["message"]   = "origTransactionId or origClientTxnId required for refund.";
                return res;
            }

            if (amount <= 0.0) {
                res["errorCode"] = "ERR_INVALID_REFUND_AMOUNT";
                res["message"]   = "Refund amount must be positive.";
                return res;
            }

            // ---- 1) Locate the original purchase row ----
            RowResult purchRes;

            if (!origTransId.empty()) {
                // Exact mapping: best / real-world style
                purchRes = posTbl.select(
                        "id","transaction_id","client_txn_id",
                        "amount","account_number","card_pan","card_scheme",
                        "merchant_id","terminal_id"
                    )
                    .where("transaction_id = :tid AND message = 'POS purchase successful'")
                    .bind("tid", origTransId)
                    .execute();
            } else {
                // Fallback: latest successful purchase for this clientTxnId
                purchRes = posTbl.select(
                        "id","transaction_id","client_txn_id",
                        "amount","account_number","card_pan","card_scheme",
                        "merchant_id","terminal_id"
                    )
                    .where("client_txn_id = :cid AND message = 'POS purchase successful'")
                    .orderBy("id DESC")
                    .limit(1)
                    .bind("cid", origClientId)
                    .execute();
            }

            if (purchRes.count() == 0) {
                res["errorCode"] = "ERR_PURCHASE_NOT_FOUND";
                res["message"]   = "Original POS purchase not found.";
                return res;
            }

            Row purchRow = purchRes.fetchOne();

            int64_t purchaseDbId     = purchRow[0].get<int64_t>();
            std::string purchaseTxnId= purchRow[1].get<std::string>();
            std::string purchaseCliId= purchRow[2].get<std::string>();
            double purchaseAmount    = purchRow[3].get<double>();
            std::string accNo        = purchRow[4].get<std::string>();
            std::string purchasePan  = purchRow[5].get<std::string>();
            std::string purchaseScheme = purchRow[6].isNull()? std::string() : purchRow[6].get<std::string>();
            std::string purchMid     = purchRow[7].get<std::string>();
            std::string purchTid     = purchRow[8].get<std::string>();

            // If both ids provided, ensure they refer to same purchase
            if (!origClientId.empty() && origClientId != purchaseCliId) {
                res["errorCode"] = "ERR_ORIGINAL_ID_MISMATCH";
                res["message"]   = "origClientTxnId does not match the original transaction.";
                return res;
            }

            // 2) Merchant / terminal consistency
            if (!merchantId.empty() && merchantId != purchMid) {
                res["errorCode"] = "ERR_MERCHANT_MISMATCH";
                res["message"]   = "Refund merchantId does not match original purchase.";
                return res;
            }
            if (!terminalId.empty() && terminalId != purchTid) {
                res["errorCode"] = "ERR_TERMINAL_MISMATCH";
                res["message"]   = "Refund terminalId does not match original purchase.";
                return res;
            }

            // 3) Optional card verification (real world: must use same card)
            if (data.contains("card")) {
                json card = data["card"];
                std::string pan    = card.value("pan", std::string());
                std::string expiry = card.value("expiry", std::string());
                std::string cvv    = card.value("cvv", std::string());

                if (!pan.empty() && pan != purchasePan) {
                    res["errorCode"] = "ERR_PAN_MISMATCH";
                    res["message"]   = "Refund PAN does not match original purchase PAN.";
                    return res;
                }

                // If expiry/cvv given you could optionally re-check against cards table;
                // for now we trust the original card + PAN.
            }

            // 4) How much already refunded for this purchase?
            RowResult sumRes = posTbl.select("IFNULL(SUM(amount),0)")
                .where("original_purchase_id = :pid AND message = 'Refund successful'")
                .bind("pid", purchaseDbId)
                .execute();

            double alreadyRefunded = 0.0;
            if (sumRes.count() > 0) {
                Row r = sumRes.fetchOne();
                alreadyRefunded = r[0].get<double>();
            }

            double remaining = purchaseAmount - alreadyRefunded;

            if (remaining <= 0.0) {
                res["errorCode"]         = "ERR_ALREADY_REFUNDED";
                res["message"]           = "Purchase already fully refunded.";
                res["purchaseAmount"]    = purchaseAmount;
                res["alreadyRefunded"]   = alreadyRefunded;
                res["remainingRefundable"] = 0.0;
                return res;
            }

            if (amount > remaining) {
                res["errorCode"]         = "ERR_REFUND_EXCEEDS_REMAINING";
                res["message"]           = "Refund amount exceeds remaining refundable amount.";
                res["purchaseAmount"]    = purchaseAmount;
                res["alreadyRefunded"]   = alreadyRefunded;
                res["remainingRefundable"] = remaining;
                return res;
            }

            // 5) Credit the original account, atomically
            RowResult accRes = accounts.select("balance")
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            if (accRes.count() == 0) {
                res["errorCode"] = "ERR_ACCOUNT_NOT_FOUND";
                res["message"]   = "Original account for purchase not found.";
                return res;
            }

            double balance = accRes.fetchOne()[0].get<double>();
            double newBal  = balance + amount;

            txStarted = true;
            sess.startTransaction();

            // Update balance
            accounts.update()
                .set("balance", newBal)
                .where("account_number = :a")
                .bind("a", accNo)
                .execute();

            // Insert refund row
            auto insRefund = posTbl.insert(
                "transaction_id","client_txn_id","merchant_id","terminal_id",
                "location","account_number","amount","fee","card_pan","card_scheme",
                "status","message","original_purchase_id"
            ).values(
                txnId,
                clientTxnId,          // new client txn id for refund
                purchMid,             // force same merchant/terminal as purchase
                purchTid,
                location,             // current POS location (or could reuse purchase location)
                accNo,
                amount,
                0.0,                  // usually no fee on refund
                purchasePan,
                purchaseScheme,
                "SUCCESS",
                "Refund successful",
                purchaseDbId
            ).execute();

            int64_t refundChildId = insRefund.getAutoIncrementValue();

            master.insert("table_name","reference_id","status")
                .values("transaction_pos", refundChildId, "SUCCESS")
                .execute();

            sess.commit();
            txStarted = false;

            res["transactionId"]      = txnId;
            res["status"]             = "SUCCESS";
            res["balanceAfter"]       = newBal;
            res["purchaseAmount"]     = purchaseAmount;
            res["alreadyRefunded"]    = alreadyRefunded + amount;
            res["remainingRefundable"]= purchaseAmount - (alreadyRefunded + amount);
            res["originalTransactionId"] = purchaseTxnId;
            return res;
        }

        // ==========================================================
        //                   Unsupported transaction type
        // ==========================================================
        res["errorCode"] = "ERR_INVALID_TYPE";
        res["message"]   = "Unsupported POS transactionType. Only PURCHASE and REFUND allowed.";
        return res;
    }
    catch (const std::exception &e) {
        res["errorCode"] = "ERR_EXCEPTION";
        res["message"]   = e.what();
        return res;
    }
}
