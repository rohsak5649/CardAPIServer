/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* PAYMENT SWITCHING ENGINE – ROUTING & ORCHESTRATION FLOW
*
* 1. PURPOSE:
*    - Acts as central routing layer for all transaction channels.
*    - Receives client requests and dispatches them to appropriate processors.
*    - Provides unified API interface for multi-channel transaction processing.
*
* 2. SUPPORTED CHANNELS:
*    - ATM       → Cash withdrawal / deposit
*    - MOBILE    → Mobile banking transactions
*    - POS       → Point-of-sale payments
*    - ECOM      → E-commerce purchase & refund
*    - QRCODE    → QR-based payments
*    - RINGPAY   → Contactless wearable payments
*    - ISSUER    → Card issuance services
*
* 3. API ENDPOINTS:
*
*    POST /transaction/initiate
*    - Main transaction entry point
*    - Accepts JSON request:
*        {
*            "channelId": "ECOM",
*            "data": { ... }
*        }
*
*    GET /health
*    - Health check endpoint
*    - Used for monitoring and uptime verification
*
* 4. REQUEST FLOW:
*
*    Step 1: Client sends HTTP POST request
*    Step 2: Request body parsed into JSON
*    Step 3: Validate required fields:
*        → channelId
*        → data
*
*    Step 4: Routing logic executed:
*        → routeRequest(channelId, data)
*
*    Step 5: Request forwarded to respective module:
*        → ATM       → processATMTransaction()
*        → MOBILE    → processMobileTransaction()
*        → POS       → processPOSTransaction()
*        → ECOM      → processECOMTransaction()
*        → ISSUER    → processIssueCard()
*        → QRCODE    → processQRCodePayment()
*        → RINGPAY   → processRingPayTransaction()
*
*    Step 6: Channel-specific business logic executed
*
*    Step 7: JSON response returned to client
*
* 5. ERROR HANDLING:
*
*    - Empty request body → HTTP 400
*    - Missing fields → HTTP 400
*    - Unsupported channel → ERR_UNKNOWN_CHANNEL
*    - Internal exception → HTTP 500 (ERR_SERVER)
*    - Unknown error → ERR_UNKNOWN
*
* 6. LOGGING:
*    - Each channel request logged to console:
*        → [ATM], [ECOM], [MOBILE], etc.
*    - Helps in debugging and tracing transaction flow
*
* 7. ARCHITECTURE DESIGN:
*
*    - Follows Dispatcher / Router Pattern
*    - Separation of concerns:
*        → Routing layer (this file)
*        → Business logic (channel modules)
*        → DB layer (Database module)
*
*    - Each channel is independently scalable
*
* 8. PERFORMANCE NOTES:
*
*    - Lightweight routing logic ensures minimal latency
*    - Supports high-frequency transaction systems
*    - Stateless request handling
*
* 9. SECURITY CONSIDERATIONS:
*
*    - Input validation enforced before routing
*    - Sensitive operations handled in downstream modules
*    - Recommended production enhancements:
*        → Authentication (JWT / OAuth)
*        → Rate limiting
*        → Request signing
*
* 10. FUTURE ENHANCEMENTS:
*
*    - Add middleware for:
*        → Logging (centralized)
*        → Authentication & authorization
*        → Rate limiting
*
*    - Add API gateway integration
*    - Add distributed tracing (Jaeger / Zipkin)
*    - Add metrics (Prometheus / Grafana)
*
*    - Convert routing to dynamic registry-based system
*      instead of static if-else chain
*
* Unauthorized modification without understanding routing,
* channel separation, and transaction orchestration is discouraged.
*
* For implementation details:
* Email: rohanavinashsakhare@gmail.com
* Mobile: +91 9112765649
*/
#include <iostream>
#include "httplib.h"
#include "json.hpp"

// ATM
#include "atm.h"
// Mobile Banking
#include "mobile.h"
// POS
#include "pos.h"
// Issue Card
#include "issue.h"
// E-Commerce
#include "ecom.h"
#include "qrcode.h"
#include "ringpay.h"
#include "Database.h"

using json = nlohmann::json;

json routeRequest(const std::string &channelId, const json &data) {
    if (channelId == "ATM") {
        std::cout << "[ATM] Processing ATM transaction\n";
        return processATMTransaction(data);
    }
    else if (channelId == "MOBILE") {
        std::cout << "[MOBILE] Processing Mobile Banking transaction\n";
        return processMobileTransaction(data);
    }
    else if (channelId == "POS") {
        std::cout << "[POS] Processing POS transaction\n";
        return processPOSTransaction(data);
    }
    else if (channelId == "ISSUER") {
        std::cout << "[ISSUER] Processing Card Issuance\n";
        return processIssueCard(data);
    }
    else if (channelId == "ECOM") {
        std::cout << "[ECOM] Processing E-Commerce transaction\n";
        return processECOMTransaction(data);
    }
    else if (channelId == "QRCODE") {
        std::cout << "[QRCODE] routing to QR processor\n";
        return processQRCodePayment(data);
    }
    else if (channelId == "RINGPAY") {
        std::cout << "[RINGPAY] Processing RingPay transaction\n";
        return processRingPayTransaction(data);
    }

    else {
        json err;
        err["errorCode"] = "ERR_UNKNOWN_CHANNEL";
        err["message"] = "Unsupported channelId";
        return err;
    }
}

int main() {

    httplib::Server svr;

    // ----------------- HEALTH CHECK ------------------
    svr.Get("/health", [](const httplib::Request&, httplib::Response &res) {
        json ok;
        ok["status"] = "UP";
        ok["message"] = "Payment Switching Engine Running";
        res.set_content(ok.dump(4), "application/json");
    });

    // ----------------- MAIN ROUTER --------------------
    svr.Post("/transaction/initiate",
        [](const httplib::Request &req, httplib::Response &res) {

            try {
                if (req.body.empty()) {
                    json err;
                    err["error"] = "Empty request body";
                    res.status = 400;
                    res.set_content(err.dump(4), "application/json");
                    return;
                }

                json request = json::parse(req.body);

                if (!request.contains("channelId") || !request.contains("data")) {
                    json err;
                    err["error"] = "Missing channelId or data";
                    res.status = 400;
                    res.set_content(err.dump(4), "application/json");
                    return;
                }

                std::string channelId = request["channelId"];
                json data = request["data"];

                json response = routeRequest(channelId, data);

                res.set_content(response.dump(4), "application/json");
            }
            catch (std::exception &e) {
                json err;
                err["errorCode"] = "ERR_SERVER";
                err["message"] = e.what();
                res.status = 500;
                res.set_content(err.dump(4), "application/json");
            }
            catch (...) {
                json err;
                err["errorCode"] = "ERR_UNKNOWN";
                err["message"] = "Unexpected server error";
                res.status = 500;
                res.set_content(err.dump(4), "application/json");
            }
        }
    );

    // ----------------- SERVER HEADER ------------------
    std::cout << "\n====================================\n";
    std::cout << " Payment Switching Engine Started    \n";
    std::cout << " Listening at http://localhost:8080 \n";
    std::cout << "------------------------------------\n";
    std::cout << " POST /transaction/initiate\n";
    std::cout << " Channels: ATM, MOBILE, POS, ECOM, ISSUER\n";
    std::cout << " GET  /health\n";
    std::cout << "====================================\n\n";

    svr.listen("0.0.0.0", 8080);

    return 0;
}
