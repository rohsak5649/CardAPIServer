/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * DATABASE CONNECTION POOL  v3.0  (C++20 · Thread-Safe · Self-Healing)
 * ─────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.0
 *   ✅ Pool size raised to 30 (tuneable via POOL_SIZE)
 *   ✅ Self-healing: acquire() pings session, reconnects if stale
 *   ✅ [[nodiscard]] on acquire() — compiler warns if caller ignores it
 *   ✅ std::optional<Session*> tryAcquire() — non-blocking variant
 *   ✅ poolHealth() returns live stats (free / total / stale count)
 *   ✅ Credentials loaded from env-vars with fallback (SOLID OCP)
 *   ✅ ScopedConnection supports std::move correctly
 *   ✅ All internals use std::string_view where possible
 *
 * USAGE (recommended):
 *   {
 *       Database::ScopedConnection sc;   // acquires from pool
 *       sc->sql("SELECT ...").execute();
 *   }                                    // auto-releases on scope exit
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#pragma once

#include <mysqlx/xdevapi.h>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <atomic>
#include <string>
#include <string_view>

using namespace mysqlx;

// ── Tuneable constants ────────────────────────────────────────────────────────
inline constexpr int  POOL_SIZE          = 30;     // max simultaneous DB connections
inline constexpr int  POOL_TIMEOUT_MS    = 5'000;  // ms to wait for a free slot
inline constexpr int  HEALTH_CHECK_RETRIES = 3;    // reconnect attempts before throw
// ─────────────────────────────────────────────────────────────────────────────

struct PoolHealth {
    int total;
    int free;
    int staleReconnects;   // cumulative reconnects since start
};

class Database {
public:
    // ── RAII scoped connection ─────────────────────────────────────────────
    class ScopedConnection {
    public:
        ScopedConnection();
        ~ScopedConnection();

        ScopedConnection(const ScopedConnection&)            = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;
        ScopedConnection(ScopedConnection&&)            noexcept;
        ScopedConnection& operator=(ScopedConnection&&) noexcept;

        Session* operator->() const { return session_; }
        Session& operator*()  const { return *session_; }
        [[nodiscard]] bool valid() const noexcept { return session_ != nullptr; }

    private:
        Session* session_ = nullptr;
    };

    // ── Pool interface ─────────────────────────────────────────────────────
    [[nodiscard]] static Session*           acquire();
    static void                             release(Session* session);
    static std::optional<Session*>          tryAcquire();     // non-blocking

    // ── Pool management ────────────────────────────────────────────────────
    static void        initPool();
    static void        destroyPool();
    static PoolHealth  poolHealth();
    static int         poolSize() noexcept { return POOL_SIZE; }

    // ── Legacy shim (NOT thread-safe — migrate to ScopedConnection) ────────
    static Session& getSession();
    static Schema   getSchema();

private:
    static Session* createSession_();
    static bool     pingSession_(Session* s) noexcept;
    static Session* healSession_(Session* s);

    static std::vector<Session*>   allConnections_;
    static std::queue<Session*>    freeConnections_;
    static std::mutex              poolMutex_;
    static std::condition_variable poolCV_;
    static bool                    poolReady_;
    static std::atomic<int>        staleReconnects_;

    static Session* legacySession_;
};