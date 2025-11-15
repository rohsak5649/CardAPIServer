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
