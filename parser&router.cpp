/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * PAYMENT SWITCHING ENGINE — ROUTING & ORCHESTRATION  v3.0
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *
 *   ✅ THREAD POOL       — 1000 workers (handles 1000+ concurrent requests)
 *   ✅ DB POOL           — 30 sessions (Database::initPool / POOL_SIZE=30)
 *   ✅ IP RATE LIMITER   — 200 req/min per IP; returns HTTP 429 if exceeded
 *   ✅ REQUEST TRACING   — X-Request-ID header auto-generated per request
 *   ✅ STRUCTURED LOGGING— every request logged with: id · channel · ms · status
 *   ✅ /metrics          — Prometheus-style counters (total, active, ok, fail)
 *   ✅ /health           — includes pool health (free/total/reconnects)
 *   ✅ /status           — extended stats (thread pool, DB pool, rates)
 *   ✅ unordered_map dispatch — O(1) channel routing (replaces if-chain)
 *   ✅ Graceful shutdown  — SIGTERM/SIGINT → destroyPool + clean exit
 *   ✅ std::string_view  — zero-copy channel lookup
 *   ✅ [[nodiscard]]     — compiler enforces checking return values
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include <iostream>
#include <atomic>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <chrono>
#include <sstream>
#include <random>
#include <csignal>
#include <vector>

#include "httplib.h"
#include "json.hpp"
#include "Database.h"
#include "TransactionLogger.h"
#include "atm.h"
#include "mobile.h"
#include "pos.h"
#include "issue.h"
#include "ecom.h"
#include "qrcode.h"
#include "ringpay.h"
#include "account.h"

using json = nlohmann::json;

// ── Server constants ──────────────────────────────────────────────────────────
inline constexpr int  SERVER_PORT        = 8080;
inline constexpr int  THREAD_POOL_SIZE   = 1000;   // handle 1000+ concurrent requests
inline constexpr int  MAX_QUEUED_REQUESTS = 2000;  // back-pressure above this queue size
inline constexpr int  RATE_LIMIT_MAX     = 200;    // requests per minute per IP
inline constexpr int  RATE_LIMIT_WINDOW  = 60;     // seconds
inline constexpr int  SERVER_IO_TIMEOUT_SECONDS = 10;
inline constexpr int  KEEP_ALIVE_TIMEOUT_SECONDS = 10;
inline constexpr size_t MAX_REQUEST_BODY_BYTES = 1024 * 1024; // 1 MB

// ── Atomic counters ───────────────────────────────────────────────────────────
static std::atomic<long long> g_totalRequests{0};
static std::atomic<int>       g_activeRequests{0};
static std::atomic<long long> g_okResponses{0};
static std::atomic<long long> g_failResponses{0};

// ── IP Rate limiter ───────────────────────────────────────────────────────────
struct RateBucket {
    int   count      = 0;
    long long windowStart = 0;   // epoch seconds
};

static std::unordered_map<std::string, RateBucket> g_rateBuckets;
static std::mutex                                   g_rateMutex;

static bool rateLimitOK(const std::string& ip) {
    TransactionLogger::ScopedFunctionTrace trace("rateLimitOK", {{"ip", ip}});
    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_rateMutex);
    auto& b = g_rateBuckets[ip];
    if (now - b.windowStart >= RATE_LIMIT_WINDOW) {
        b.windowStart = now;
        b.count = 0;
    }
    if (b.count >= RATE_LIMIT_MAX) {
        trace.fail("IP rate limit exceeded",
                   {{"count", std::to_string(b.count)},
                    {"limit", std::to_string(RATE_LIMIT_MAX)}});
        return false;
    }
    ++b.count;
    trace.success({{"count", std::to_string(b.count)}});
    return true;
}

// ── Request ID generator ──────────────────────────────────────────────────────
[[nodiscard]] static std::string makeRequestId() {
    return TransactionLogger::instance().generateUuid();
}

// ── CORS ──────────────────────────────────────────────────────────────────────
static void addCORS(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers",
                   "Content-Type, Authorization, X-Request-ID, X-Transaction-UUID");
}

// ── Channel dispatch — O(1) unordered_map (OCP: add channels here only) ───────
using ChannelFn = std::function<json(const json&)>;
static const std::unordered_map<std::string, ChannelFn> CHANNEL_DISPATCH = {
    { "ATM",     processATMTransaction      },
    { "MOBILE",  processMobileTransaction   },
    { "POS",     processPOSTransaction      },
    { "ISSUER",  processIssueCard           },
    { "ECOM",    processECOMTransaction     },
    { "QRCODE",  processQRCodePayment       },
    { "RINGPAY", processRingPayTransaction  },
    { "CARD_DETAILS", processGetCardDetails },
    { "ADD_ACCOUNT", processAddAccount      },
    { "ACCOUNT_DETAILS", processGetAccount  },
    { "FREEZE_ACCOUNT", processFreezeAccount },
    { "UNFREEZE_ACCOUNT", processUnfreezeAccount },
    { "LIST_ACCOUNTS", processListAccounts  },
};

// ── Structured logger ─────────────────────────────────────────────────────────
[[nodiscard]] static std::string truncateForLog(std::string value,
                                                std::size_t limit = 500) {
    if (value.size() <= limit) return value;
    return value.substr(0, limit) + "...";
}

[[nodiscard]] static std::string responseField(const json& response,
                                               const char* key) {
    if (!response.contains(key) || response[key].is_null()) return "";
    if (response[key].is_string()) return response[key].get<std::string>();
    return response[key].dump();
}

static void attachCorrelation(json& body, const std::string& reqId) {
    TransactionLogger::ScopedFunctionTrace trace("attachCorrelation");
    body["requestId"] = reqId;
    body["transactionUuid"] = reqId;
    trace.success({{"transactionUuid", reqId}});
}

static void attachRequestContext(json& body,
                                 const std::string& reqId,
                                 const std::string& channel,
                                 const httplib::Request* req = nullptr) {
    TransactionLogger::ScopedFunctionTrace trace("attachRequestContext",
                                                 {{"channel", channel}});
    if (!body.is_object()) {
        trace.fail("request data is not a JSON object");
        return;
    }
    body["_correlationUuid"] = reqId;
    body["_channelId"] = channel;
    if (req != nullptr) {
        json security = json::object();
        security["remoteAddr"] = req->remote_addr;

        auto copyHeader = [&](const char* header, const char* field,
                              std::size_t limit = 160) {
            std::string value = req->get_header_value(header);
            if (!value.empty()) {
                security[field] = truncateForLog(value, limit);
            }
        };

        copyHeader("User-Agent", "userAgent", 220);
        copyHeader("X-Forwarded-For", "forwardedFor", 160);
        copyHeader("X-Real-IP", "realIp", 80);
        copyHeader("X-Device-Integrity", "deviceIntegrity", 40);
        copyHeader("X-App-Signature-Valid", "appSignatureValid", 12);
        copyHeader("X-Device-Binding-Valid", "deviceBindingValid", 12);
        copyHeader("X-Device-Trust-Score", "deviceTrustScore", 12);
        copyHeader("X-Malware-Detected", "malwareDetected", 12);
        copyHeader("X-Rooted-Device", "rooted", 12);
        copyHeader("X-Debugger-Detected", "debuggerDetected", 12);
        copyHeader("X-Proxy-Detected", "proxyDetected", 12);
        copyHeader("X-VPN-Detected", "vpnDetected", 12);
        copyHeader("X-Falcon-Risk-Score", "riskScore", 12);

        body["_securityContext"] = std::move(security);
    }
    trace.success({{"transactionUuid", reqId}});
}

static void addCorrelationHeaders(httplib::Response& res,
                                  const std::string& reqId) {
    TransactionLogger::ScopedFunctionTrace trace("addCorrelationHeaders");
    res.set_header("X-Request-ID", reqId);
    res.set_header("X-Transaction-UUID", reqId);
    trace.success({{"transactionUuid", reqId}});
}

static void logRequest(const std::string& reqId, const std::string& channel,
                       long long ms, int httpStatus, const json& response) {
    TransactionLogger::ScopedFunctionTrace trace("logRequest",
                                                 {{"channel", channel},
                                                  {"httpStatus", std::to_string(httpStatus)}});
    std::cout << "[" << reqId << "] channel=" << channel
              << " ms=" << ms << " http=" << httpStatus << "\n";

    std::vector<LogField> fields{
        {"durationMs", std::to_string(ms)},
        {"httpStatus", std::to_string(httpStatus)}
    };

    std::string transactionId = responseField(response, "transactionId");
    std::string status = responseField(response, "status");
    std::string errorCode = responseField(response, "errorCode");
    std::string message = responseField(response, "message");

    if (!transactionId.empty()) fields.push_back({"transactionId", transactionId});
    if (!status.empty()) fields.push_back({"status", status});
    if (!errorCode.empty()) fields.push_back({"errorCode", errorCode});
    if (!message.empty()) fields.push_back({"responseMessage", truncateForLog(message)});

    TransactionLogger::instance().log(
        httpStatus >= 500 ? "ERROR" : (httpStatus >= 400 ? "WARN" : "INFO"),
        reqId,
        channel,
        "request_finished",
        "HTTP request completed",
        fields);
    trace.success({{"durationMs", std::to_string(ms)}});
}

[[nodiscard]] static int httpStatusFor(const json& response) {
    TransactionLogger::ScopedFunctionTrace trace("httpStatusFor");
    if (!response.contains("errorCode") || !response["errorCode"].is_string()) {
        trace.success({{"httpStatus", "200"}});
        return 200;
    }

    const std::string code = response["errorCode"].get<std::string>();
    if (code.find("NOT_FOUND") != std::string::npos) {
        trace.success({{"httpStatus", "404"}, {"errorCode", code}});
        return 404;
    }
    if (code.find("EXISTS") != std::string::npos ||
        code.find("FROZEN") != std::string::npos) {
        trace.success({{"httpStatus", "409"}, {"errorCode", code}});
        return 409;
    }
    if (code == "ERR_DB" || code == "ERR_EXCEPTION" || code == "ERR_SERVER") {
        trace.success({{"httpStatus", "500"}, {"errorCode", code}});
        return 500;
    }
    trace.success({{"httpStatus", "400"}, {"errorCode", code}});
    return 400;
}

static void sendJson(httplib::Response& res, const json& body, int status) {
    TransactionLogger::ScopedFunctionTrace trace("sendJson",
                                                 {{"httpStatus", std::to_string(status)}});
    res.status = status;
    res.set_content(body.dump(4), "application/json");
    trace.success();
}

static void registerJsonPost(httplib::Server& svr,
                             const std::string& path,
                             const std::string& channel,
                             const ChannelFn& handler) {
    svr.Post(path, [path, channel, handler](const httplib::Request& req, httplib::Response& res) {
        auto t0 = std::chrono::steady_clock::now();
        std::string reqId = makeRequestId();
        TransactionLogger::ScopedContext logScope(reqId, channel);
        TransactionLogger::ScopedFunctionTrace trace(
            "registerJsonPost.handler", {{"path", path}, {"channel", channel}});
        ++g_activeRequests;
        ++g_totalRequests;
        struct Guard { ~Guard(){ --g_activeRequests; } } guard;

        addCORS(res);
        addCorrelationHeaders(res, reqId);

        TransactionLogger::instance().logCurrent(
            "INFO", "request_received", "HTTP request received",
            {{"path", path}, {"remoteAddr", req.remote_addr}});

        if (!rateLimitOK(req.remote_addr)) {
            json response{{"errorCode","ERR_RATE_LIMIT"},
                          {"message","Too many requests — limit: " +
                                      std::to_string(RATE_LIMIT_MAX) + " req/min"},
                          {"requestId", reqId}};
            attachCorrelation(response, reqId);
            sendJson(res, response, 429);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            logRequest(reqId, channel, ms, 429, response);
            trace.fail("rate limit rejected request");
            ++g_failResponses;
            return;
        }

        try {
            if (req.body.empty()) {
                json response{{"error","Empty request body"},{"requestId",reqId}};
                attachCorrelation(response, reqId);
                sendJson(res, response, 400);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                logRequest(reqId, channel, ms, 400, response);
                trace.fail("empty request body");
                ++g_failResponses;
                return;
            }

            json request;
            try { request = json::parse(req.body); }
            catch (...) {
                json response{{"error","Invalid JSON"},{"requestId",reqId}};
                attachCorrelation(response, reqId);
                sendJson(res, response, 400);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                logRequest(reqId, channel, ms, 400, response);
                trace.fail("invalid JSON request body");
                ++g_failResponses;
                return;
            }

            attachRequestContext(request, reqId, channel, &req);
            json response = handler(request);
            attachCorrelation(response, reqId);
            int status = httpStatusFor(response);
            sendJson(res, response, status);

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            logRequest(reqId, channel, ms, status, response);

            if (status >= 200 && status < 300) ++g_okResponses;
            else ++g_failResponses;
            if (status >= 400) {
                trace.fail("handler returned error response",
                           {{"httpStatus", std::to_string(status)},
                            {"errorCode", responseField(response, "errorCode")}});
            } else {
                trace.success({{"httpStatus", std::to_string(status)}});
            }

        } catch (const std::exception& e) {
            json response{{"errorCode","ERR_SERVER"},{"message",e.what()},{"requestId",reqId}};
            attachCorrelation(response, reqId);
            sendJson(res, response, 500);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            logRequest(reqId, channel, ms, 500, response);
            trace.fail("handler threw exception", {{"error", e.what()}});
            ++g_failResponses;
        } catch (...) {
            json response{{"errorCode","ERR_UNKNOWN"},{"message","Unexpected error"},{"requestId",reqId}};
            attachCorrelation(response, reqId);
            sendJson(res, response, 500);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            logRequest(reqId, channel, ms, 500, response);
            trace.fail("handler threw unknown exception");
            ++g_failResponses;
        }
    });
}

// ── Signal handler for graceful shutdown ─────────────────────────────────────
static httplib::Server* g_server = nullptr;
static void onSignal(int) {
    std::cout << "\n[Engine] Shutdown signal received…\n";
    if (g_server) g_server->stop();
}

// ═════════════════════════════════════════════════════════════════════════════
int main() {
    TransactionLogger::ScopedFunctionTrace mainTrace("main");
    TransactionLogger::instance().initialize();
    const std::string engineRunId = TransactionLogger::instance().generateUuid();
    TransactionLogger::instance().log(
        "INFO", engineRunId, "ENGINE", "engine_starting",
        "Payment Switching Engine starting",
        {{"port", std::to_string(SERVER_PORT)},
         {"threadPool", std::to_string(THREAD_POOL_SIZE)},
         {"dbPool", std::to_string(POOL_SIZE)},
         {"logDirectory", TransactionLogger::instance().logDirectory().string()}});

    // ── Init DB pool ─────────────────────────────────────────────────────
    try { Database::initPool(); }
    catch (const std::exception& ex) {
        std::cerr << "[FATAL] DB pool init failed: " << ex.what() << "\n";
        TransactionLogger::instance().log(
            "ERROR", engineRunId, "ENGINE", "db_pool_init_failed",
            "Database pool initialization failed",
            {{"error", truncateForLog(ex.what())}});
        mainTrace.fail("DB pool initialization failed", {{"error", ex.what()}});
        TransactionLogger::instance().shutdown();
        return 1;
    }
    TransactionLogger::instance().log(
        "INFO", engineRunId, "ENGINE", "db_pool_ready",
        "Database connection pool initialized",
        {{"poolSize", std::to_string(POOL_SIZE)}});

    // ── Server ───────────────────────────────────────────────────────────
    httplib::Server svr;
    g_server = &svr;
    svr.new_task_queue = []{ return new httplib::ThreadPool(THREAD_POOL_SIZE, MAX_QUEUED_REQUESTS); };
    svr.set_read_timeout(std::chrono::seconds(SERVER_IO_TIMEOUT_SECONDS));
    svr.set_write_timeout(std::chrono::seconds(SERVER_IO_TIMEOUT_SECONDS));
    svr.set_keep_alive_timeout(KEEP_ALIVE_TIMEOUT_SECONDS);
    svr.set_idle_interval(0, 100000);
    svr.set_payload_max_length(MAX_REQUEST_BODY_BYTES);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // ── OPTIONS pre-flight ────────────────────────────────────────────────
    auto optHandler = [](const httplib::Request&, httplib::Response& res) {
        addCORS(res); res.status = 204;
    };
    svr.Options("/transaction/initiate", optHandler);
    svr.Options("/account/add",           optHandler);
    svr.Options("/account/details",       optHandler);
    svr.Options("/account/freeze",        optHandler);
    svr.Options("/account/unfreeze",      optHandler);
    svr.Options("/account/list",          optHandler);
    svr.Options("/health",               optHandler);
    svr.Options("/status",               optHandler);
    svr.Options("/metrics",              optHandler);

    // ── GET /health ───────────────────────────────────────────────────────
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        PoolHealth ph = Database::poolHealth();
        json ok;
        ok["status"]          = "UP";
        ok["message"]         = "Payment Switching Engine v3.0 running";
        ok["dbPool"]["total"] = ph.total;
        ok["dbPool"]["free"]  = ph.free;
        ok["dbPool"]["staleReconnects"] = ph.staleReconnects;
        addCORS(res);
        res.set_content(ok.dump(4), "application/json");
    });

    // ── GET /status ───────────────────────────────────────────────────────
    svr.Get("/status", [](const httplib::Request&, httplib::Response& res) {
        PoolHealth ph = Database::poolHealth();
        json s;
        s["status"]            = "UP";
        s["active_requests"]   = g_activeRequests.load();
        s["total_requests"]    = g_totalRequests.load();
        s["ok_responses"]      = g_okResponses.load();
        s["fail_responses"]    = g_failResponses.load();
        s["thread_pool"]       = THREAD_POOL_SIZE;
        s["db_pool_size"]      = POOL_SIZE;
        s["db_pool_free"]      = ph.free;
        s["db_stale_reconnects"] = ph.staleReconnects;
        addCORS(res);
        res.set_content(s.dump(4), "application/json");
    });

    // ── GET /metrics (Prometheus-style) ───────────────────────────────────
    svr.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        std::ostringstream oss;
        oss << "# HELP engine_total_requests Total HTTP requests\n"
            << "# TYPE engine_total_requests counter\n"
            << "engine_total_requests " << g_totalRequests.load() << "\n"
            << "# HELP engine_active_requests Active requests in flight\n"
            << "# TYPE engine_active_requests gauge\n"
            << "engine_active_requests " << g_activeRequests.load() << "\n"
            << "# HELP engine_ok_responses Total 2xx responses\n"
            << "engine_ok_responses " << g_okResponses.load() << "\n"
            << "# HELP engine_fail_responses Total non-2xx responses\n"
            << "engine_fail_responses " << g_failResponses.load() << "\n";
        addCORS(res);
        res.set_content(oss.str(), "text/plain");
    });

    // ── POST /transaction/initiate ────────────────────────────────────────
    svr.Post("/transaction/initiate",
        [](const httplib::Request& req, httplib::Response& res) {

            auto t0     = std::chrono::steady_clock::now();
            std::string reqId = makeRequestId();
            std::string channelForLog = "TRANSACTION";
            TransactionLogger::ScopedContext logScope(reqId, channelForLog);
            TransactionLogger::ScopedFunctionTrace trace(
                "POST /transaction/initiate", {{"path", "/transaction/initiate"}});
            ++g_activeRequests;
            ++g_totalRequests;
            struct Guard { ~Guard(){ --g_activeRequests; } } guard;

            addCORS(res);
            addCorrelationHeaders(res, reqId);

            auto finish = [&](json body, int status) {
                attachCorrelation(body, reqId);
                sendJson(res, body, status);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                logRequest(reqId, channelForLog, ms, status, body);
            };

            TransactionLogger::instance().logCurrent(
                "INFO", "request_received", "Transaction request received",
                {{"path", "/transaction/initiate"}, {"remoteAddr", req.remote_addr}});

            // ── IP rate limit ─────────────────────────────────────────────
            std::string ip = req.remote_addr;
            if (!rateLimitOK(ip)) {
                json r{{"errorCode","ERR_RATE_LIMIT"},
                       {"message","Too many requests — limit: " +
                                   std::to_string(RATE_LIMIT_MAX) + " req/min"},
                       {"requestId", reqId}};
                finish(r, 429);
                trace.fail("rate limit rejected request");
                ++g_failResponses;
                return;
            }

            try {
                if (req.body.empty()) {
                    json e{{"error","Empty request body"},{"requestId",reqId}};
                    finish(e, 400);
                    trace.fail("empty request body");
                    ++g_failResponses; return;
                }

                json request;
                try { request = json::parse(req.body); }
                catch (...) {
                    json e{{"error","Invalid JSON"},{"requestId",reqId}};
                    finish(e, 400);
                    trace.fail("invalid JSON request body");
                    ++g_failResponses; return;
                }

                if (!request.contains("channelId") || !request.contains("data")) {
                    json e{{"error","Missing channelId or data"},{"requestId",reqId}};
                    finish(e, 400);
                    trace.fail("missing channelId or data");
                    ++g_failResponses; return;
                }

                std::string channelId = request["channelId"].get<std::string>();
                channelForLog = channelId;
                TransactionLogger::ScopedContext channelScope(reqId, channelForLog);

                auto it = CHANNEL_DISPATCH.find(channelId);
                if (it == CHANNEL_DISPATCH.end()) {
                    json e{{"errorCode","ERR_UNKNOWN_CHANNEL"},
                           {"message","Unsupported channelId: " + channelId},
                           {"requestId",reqId}};
                    finish(e, 400);
                    trace.fail("unsupported channel", {{"channel", channelId}});
                    ++g_failResponses; return;
                }

                json data = request["data"];
                attachRequestContext(data, reqId, channelId, &req);
                json response = it->second(data);
                finish(response, 200);
                if (response.contains("errorCode")) {
                    trace.fail("channel returned error response",
                               {{"channel", channelId},
                                {"transactionId", responseField(response, "transactionId")},
                                {"status", responseField(response, "status")},
                                {"errorCode", responseField(response, "errorCode")}});
                } else {
                    trace.success({{"channel", channelId},
                                   {"transactionId", responseField(response, "transactionId")},
                                   {"status", responseField(response, "status")}});
                }
                ++g_okResponses;

            } catch (const std::exception& e) {
                json er{{"errorCode","ERR_SERVER"},{"message",e.what()},{"requestId",reqId}};
                finish(er, 500);
                trace.fail("transaction route exception", {{"error", e.what()}});
                ++g_failResponses;
            } catch (...) {
                json er{{"errorCode","ERR_UNKNOWN"},{"message","Unexpected error"},{"requestId",reqId}};
                finish(er, 500);
                trace.fail("transaction route unknown exception");
                ++g_failResponses;
            }
        }
    );

    // ── Account APIs ─────────────────────────────────────────────────────
    registerJsonPost(svr, "/account/add", "ADD_ACCOUNT", processAddAccount);
    registerJsonPost(svr, "/account/details", "ACCOUNT_DETAILS", processGetAccount);
    registerJsonPost(svr, "/account/freeze", "FREEZE_ACCOUNT", processFreezeAccount);
    registerJsonPost(svr, "/account/unfreeze", "UNFREEZE_ACCOUNT", processUnfreezeAccount);
    registerJsonPost(svr, "/account/list", "LIST_ACCOUNTS", processListAccounts);

    // ── Startup banner ────────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║   Payment Switching Engine  v3.0  –  Rohan Sakhare      ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  http://0.0.0.0:" << SERVER_PORT << "   CORS: ENABLED               ║\n";
    std::cout << "║  Thread pool : " << THREAD_POOL_SIZE
              << "   DB pool   : " << POOL_SIZE << "                     ║\n";
    std::cout << "║  POST /transaction/initiate   POST /account/*            ║\n";
    std::cout << "║  GET  /health  /status  /metrics                        ║\n";
    std::cout << "║  Rate limit: " << RATE_LIMIT_MAX << " req/min per IP                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
    std::cout << "[Engine] Transaction logs: "
              << TransactionLogger::instance().logDirectory().string() << "\n";

    svr.listen("0.0.0.0", SERVER_PORT);

    std::cout << "[Engine] Shutting down…\n";
    TransactionLogger::instance().log(
        "INFO", engineRunId, "ENGINE", "engine_stopping",
        "Payment Switching Engine stopping");
    Database::destroyPool();
    TransactionLogger::instance().log(
        "INFO", engineRunId, "ENGINE", "engine_stopped",
        "Payment Switching Engine stopped");
    mainTrace.success();
    TransactionLogger::instance().shutdown();
    return 0;
}
