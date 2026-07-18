/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * DATABASE CONNECTION POOL — IMPLEMENTATION  v3.0
 * ─────────────────────────────────────────────────────────────────────
 * NEW IN v3.0
 *   • acquire()    — pings session before returning; heals stale ones
 *   • tryAcquire() — non-blocking; returns std::nullopt if pool empty
 *   • poolHealth() — returns live PoolHealth struct
 *   • Credentials  — read from env vars: DB_HOST / DB_PORT / DB_USER /
 *                    DB_PASS / DB_NAME  (falls back to dev defaults)
 *
 * Contact: rohanavinashsakhare@gmail.com  |  +91 9112765649
 */

#include "Database.h"
#include "DbConfig.h"
#include "TransactionLogger.h"
#include <iostream>
#include <cstdlib>
#include <thread>

// ── Static member definitions ───────────────────────────────────────────────────
int                     Database::poolSizeRuntime_ = POOL_SIZE_DEFAULT;
DbCredentials           Database::creds_;   // populated by DbConfig::load() in initPool()
std::vector<Session*>   Database::allConnections_;
std::queue<Session*>    Database::freeConnections_;
std::mutex              Database::poolMutex_;
std::condition_variable Database::poolCV_;
bool                    Database::poolReady_    = false;
std::atomic<int>        Database::staleReconnects_{0};
Session*                Database::legacySession_ = nullptr;

// ── NOTE: DB credentials are loaded from db.ini via DbConfig::load() ─────────
// The env() helper has been removed — no credential fallbacks exist in source.

// ── Internal: create a fresh authenticated Session ─────────────────────────────────
// Uses credentials loaded from db.ini by DbConfig::load() — no hardcoded values.
Session* Database::createSession_() {
    TransactionLogger::ScopedFunctionTrace trace("Database::createSession_");

    Session* s = new Session(creds_.host, creds_.port, creds_.user, creds_.pass);
    s->sql("USE " + creds_.dbName).execute();
    trace.success({{"host", creds_.host},
                   {"port", std::to_string(creds_.port)},
                   {"database", creds_.dbName}});
    return s;
}

// ── Ping a session: returns true if alive ────────────────────────────────────
bool Database::pingSession_(Session* s) noexcept {
    TransactionLogger::ScopedFunctionTrace trace("Database::pingSession_");
    try {
        s->sql("SELECT 1").execute();
        trace.success({{"alive", "true"}});
        return true;
    }
    catch (...) {
        trace.fail("session ping failed");
        return false;
    }
}

// ── Heal: close stale session, open fresh one ────────────────────────────────
Session* Database::healSession_(Session* old) {
    TransactionLogger::ScopedFunctionTrace trace("Database::healSession_");
    try { old->close(); } catch (...) {}
    delete old;
    ++staleReconnects_;

    for (int i = 0; i < HEALTH_CHECK_RETRIES; ++i) {
        try { return createSession_(); }
        catch (const std::exception& ex) {
            std::cerr << "[DB] Reconnect attempt " << (i+1) << " failed: " << ex.what() << "\n";
            trace.checkpoint("reconnect_attempt_failed", "DB reconnect attempt failed",
                             {{"attempt", std::to_string(i + 1)}, {"error", ex.what()}});
        }
    }
    trace.fail("cannot reconnect stale database session");
    throw std::runtime_error("[DB] Cannot reconnect after " +
                             std::to_string(HEALTH_CHECK_RETRIES) + " attempts");
}

// ── initPool() ────────────────────────────────────────────────────────────────
void Database::initPool() {
    // ── Step 1: Load credentials from encrypted db.ini (once, before pool opens) ─
    //   DbConfig::load() will:
    //     • Auto-encrypt plain-text file on first run
    //     • Verify GCM tag and abort with an error if file was tampered
    //     • Throw if the file is missing (user must create db.ini first)
    try {
        creds_ = DbConfig::load(DB_CONFIG_PATH);
    } catch (const std::exception& ex) {
        std::cerr << "\n[DB] FATAL: Failed to load database configuration.\n"
                  << "       Reason: " << ex.what() << "\n\n";
        throw;  // propagate — cannot start without credentials
    }

    // ── Step 2: Determine pool size (env-var override, non-sensitive) ────────
    int size = POOL_SIZE_DEFAULT;
    if (const char* envVal = std::getenv("DB_POOL_SIZE")) {
        try {
            size = std::stoi(envVal);
        } catch (...) {
            // Ignore parse errors, fall back to default
        }
    }
    if (size < 1) size = 1;
    if (size > POOL_SIZE_MAX) size = POOL_SIZE_MAX;
    poolSizeRuntime_ = size;

    TransactionLogger::ScopedFunctionTrace trace("Database::initPool",
                                                 {{"poolSize", std::to_string(poolSizeRuntime_)}});
    std::lock_guard<std::mutex> lk(poolMutex_);
    if (poolReady_) {
        trace.success({{"status", "already_ready"}});
        return;
    }

    std::cout << "[DB] Initialising connection pool (" << poolSizeRuntime_ << " sessions)…\n";
    allConnections_.reserve(poolSizeRuntime_);

    int createdCount = 0;
    for (int i = 0; i < poolSizeRuntime_; ++i) {
        Session* s = nullptr;
        for (int attempt = 0; attempt < POOL_CONNECT_RETRIES; ++attempt) {
            try {
                s = createSession_();
                break;
            } catch (const std::exception& ex) {
                std::cerr << "[DB] Failed to create connection " << i << " (attempt " << (attempt + 1) << "): " << ex.what() << "\n";
                if (attempt + 1 < POOL_CONNECT_RETRIES) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(POOL_CONNECT_DELAY_MS));
                }
            }
        }
        if (s) {
            allConnections_.push_back(s);
            freeConnections_.push(s);
            createdCount++;
        } else {
            std::cerr << "[DB] Permanent failure creating connection slot " << i << " after " << POOL_CONNECT_RETRIES << " attempts.\n";
        }
    }

    if (createdCount == 0) {
        trace.fail("failed to initialize any database connections in pool");
        throw std::runtime_error("[DB] Failed to initialize any database connections in pool");
    }

    if (createdCount < poolSizeRuntime_) {
        std::cerr << "[DB] Warning: Only initialized " << createdCount << " out of " << poolSizeRuntime_ << " connections.\n";
    }

    poolReady_ = true;
    std::cout << "[DB] Pool ready — " << createdCount << " connections available.\n";
    trace.success({{"connections", std::to_string(createdCount)}});
}

// ── destroyPool() ────────────────────────────────────────────────────────────
void Database::destroyPool() {
    TransactionLogger::ScopedFunctionTrace trace("Database::destroyPool");
    std::lock_guard<std::mutex> lk(poolMutex_);

    // Drain freeConnections_ queue in O(1) via swap (avoids N pop() calls)
    { std::queue<Session*> empty; std::swap(freeConnections_, empty); }

    // Close and delete all sessions owned by the pool
    for (Session* s : allConnections_) {
        if (!s) continue;  // skip nullptr slots from heal-failure
        try { s->close(); } catch (...) {}
        delete s;
    }
    allConnections_.clear();
    poolReady_ = false;
    std::cout << "[DB] Pool destroyed. Total stale reconnects: "
              << staleReconnects_.load() << "\n";
    trace.success({{"staleReconnects", std::to_string(staleReconnects_.load())}});
}

// ── acquire() — blocks up to POOL_TIMEOUT_MS, heals stale sessions ───────────
Session* Database::acquire() {
    TransactionLogger::ScopedFunctionTrace trace("Database::acquire");
    std::unique_lock<std::mutex> lk(poolMutex_);

    bool ok = poolCV_.wait_for(lk,
        std::chrono::milliseconds(POOL_TIMEOUT_MS),
        [] { return !Database::freeConnections_.empty(); });

    if (!ok || freeConnections_.empty()) {
        trace.fail("database pool exhausted",
                   {{"timeoutMs", std::to_string(POOL_TIMEOUT_MS)}});
        throw std::runtime_error("[DB] Pool exhausted — no free connection within timeout");
    }

    Session* s = freeConnections_.front();
    freeConnections_.pop();
    trace.checkpoint("session_checked_out", "Database session checked out",
                     {{"freeConnections", std::to_string(freeConnections_.size())}});
    lk.unlock();   // release lock before potentially slow ping

    // Health check — heal if stale
    if (!pingSession_(s)) {
        std::cerr << "[DB] Stale session detected, reconnecting…\n";
        trace.checkpoint("stale_session", "Stale database session detected");
        Session* stale = s;
        Session* healed = nullptr;
        try {
            healed = healSession_(stale);
        } catch (...) {
            // Heal failed — remove the dead pointer from allConnections_
            // so the pool doesn't hold a dangling reference.
            {
                std::lock_guard<std::mutex> lk2(poolMutex_);
                for (auto& ptr : allConnections_) {
                    if (ptr == stale) {
                        ptr = nullptr;
                        break;
                    }
                }
            }
            trace.fail("session heal failed — connection slot nulled");
            throw; // propagate so caller gets a clean error
        }

        {
            std::lock_guard<std::mutex> lk2(poolMutex_);
            for (auto& ptr : allConnections_) {
                if (ptr == stale) {
                    ptr = healed;
                    break;
                }
            }
        }
        s = healed;
    }
    trace.success();
    return s;
}

// ── tryAcquire() — non-blocking ───────────────────────────────────────────────
std::optional<Session*> Database::tryAcquire() {
    TransactionLogger::ScopedFunctionTrace trace("Database::tryAcquire");
    std::lock_guard<std::mutex> lk(poolMutex_);
    if (freeConnections_.empty()) {
        trace.fail("no free database session available");
        return std::nullopt;
    }
    Session* s = freeConnections_.front();
    freeConnections_.pop();
    trace.success({{"freeConnections", std::to_string(freeConnections_.size())}});
    return s;
}

// ── release() ────────────────────────────────────────────────────────────────
void Database::release(Session* session) {
    TransactionLogger::ScopedFunctionTrace trace("Database::release");
    if (!session) {
        // This can happen if healSession_ failed and nulled the slot.
        // Do NOT push nullptr back into freeConnections_ — that would crash
        // the next caller who acquires it.
        trace.fail("null database session release requested — slot dropped");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        freeConnections_.push(session);
        trace.checkpoint("session_returned", "Database session returned",
                         {{"freeConnections", std::to_string(freeConnections_.size())}});
    }
    poolCV_.notify_one();
    trace.success();
}

// ── poolHealth() ─────────────────────────────────────────────────────────────
PoolHealth Database::poolHealth() {
    TransactionLogger::ScopedFunctionTrace trace("Database::poolHealth");
    std::lock_guard<std::mutex> lk(poolMutex_);
    PoolHealth health{
        static_cast<int>(allConnections_.size()),
        static_cast<int>(freeConnections_.size()),
        staleReconnects_.load()
    };
    trace.success({{"total", std::to_string(health.total)},
                   {"free", std::to_string(health.free)},
                   {"staleReconnects", std::to_string(health.staleReconnects)}});
    return health;
}

// ── ScopedConnection ─────────────────────────────────────────────────────────
Database::ScopedConnection::ScopedConnection()
    : session_(Database::acquire()) {
    TransactionLogger::instance().logCurrent(
        "DEBUG", "db_scoped_connection_acquired", "Scoped DB connection acquired");
}

Database::ScopedConnection::~ScopedConnection() {
    if (session_) {
        TransactionLogger::instance().logCurrent(
            "DEBUG", "db_scoped_connection_released", "Scoped DB connection released");
        Database::release(session_);
    }
}

Database::ScopedConnection::ScopedConnection(ScopedConnection&& o) noexcept
    : session_(o.session_) { o.session_ = nullptr; }

Database::ScopedConnection& Database::ScopedConnection::operator=(ScopedConnection&& o) noexcept {
    if (this != &o) {
        if (session_) Database::release(session_);
        session_   = o.session_;
        o.session_ = nullptr;
    }
    return *this;
}

// ── Legacy shim ───────────────────────────────────────────────────────────────
// NOTE: legacySession_ is intentionally NOT part of the pool. It is kept for
// backward-compatibility only. It is thread-safe via the pool mutex below.
Session& Database::getSession() {
    TransactionLogger::ScopedFunctionTrace trace("Database::getSession");
    // Double-checked locking to avoid races on the legacy singleton session.
    if (!legacySession_) {
        std::lock_guard<std::mutex> lk(poolMutex_);
        if (!legacySession_) {
            legacySession_ = createSession_();
        }
    }
    trace.success();
    return *legacySession_;
}

Schema Database::getSchema() {
    TransactionLogger::ScopedFunctionTrace trace("Database::getSchema");
    // creds_ is already loaded in initPool() — use it directly.
    Schema schema = getSession().getSchema(creds_.dbName);
    trace.success({{"database", creds_.dbName}});
    return schema;
}
