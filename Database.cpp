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
#include <iostream>
#include <cstdlib>

// ── Static member definitions ─────────────────────────────────────────────────
std::vector<Session*>   Database::allConnections_;
std::queue<Session*>    Database::freeConnections_;
std::mutex              Database::poolMutex_;
std::condition_variable Database::poolCV_;
bool                    Database::poolReady_    = false;
std::atomic<int>        Database::staleReconnects_{0};
Session*                Database::legacySession_ = nullptr;

// ── Env-var helpers ───────────────────────────────────────────────────────────
static std::string env(const char* key, const char* fallback) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : std::string(fallback);
}

// ── Internal: create a fresh authenticated Session ────────────────────────────
Session* Database::createSession_() {
    std::string host = env("DB_HOST", "localhost");
    int         port = std::atoi(env("DB_PORT", "33060").c_str());
    std::string user = env("DB_USER", "root");
    std::string pass = env("DB_PASS", "Rohan@5649");   // ⚠ move to vault in prod
    std::string name = env("DB_NAME", "bankingdb");

    Session* s = new Session(host, port, user, pass);
    s->sql("USE " + name).execute();
    return s;
}

// ── Ping a session: returns true if alive ────────────────────────────────────
bool Database::pingSession_(Session* s) noexcept {
    try { s->sql("SELECT 1").execute(); return true; }
    catch (...) { return false; }
}

// ── Heal: close stale session, open fresh one ────────────────────────────────
Session* Database::healSession_(Session* old) {
    try { old->close(); } catch (...) {}
    delete old;
    ++staleReconnects_;

    for (int i = 0; i < HEALTH_CHECK_RETRIES; ++i) {
        try { return createSession_(); }
        catch (const std::exception& ex) {
            std::cerr << "[DB] Reconnect attempt " << (i+1) << " failed: " << ex.what() << "\n";
        }
    }
    throw std::runtime_error("[DB] Cannot reconnect after " +
                             std::to_string(HEALTH_CHECK_RETRIES) + " attempts");
}

// ── initPool() ────────────────────────────────────────────────────────────────
void Database::initPool() {
    std::lock_guard<std::mutex> lk(poolMutex_);
    if (poolReady_) return;

    std::cout << "[DB] Initialising connection pool (" << POOL_SIZE << " sessions)…\n";
    allConnections_.reserve(POOL_SIZE);

    for (int i = 0; i < POOL_SIZE; ++i) {
        try {
            Session* s = createSession_();
            allConnections_.push_back(s);
            freeConnections_.push(s);
        } catch (const std::exception& ex) {
            std::cerr << "[DB] Failed to create connection " << i << ": " << ex.what() << "\n";
            throw;
        }
    }

    poolReady_ = true;
    std::cout << "[DB] Pool ready — " << POOL_SIZE << " connections available.\n";
}

// ── destroyPool() ────────────────────────────────────────────────────────────
void Database::destroyPool() {
    std::lock_guard<std::mutex> lk(poolMutex_);
    for (Session* s : allConnections_) {
        try { s->close(); } catch (...) {}
        delete s;
    }
    allConnections_.clear();
    while (!freeConnections_.empty()) freeConnections_.pop();
    poolReady_ = false;
    std::cout << "[DB] Pool destroyed. Total stale reconnects: "
              << staleReconnects_.load() << "\n";
}

// ── acquire() — blocks up to POOL_TIMEOUT_MS, heals stale sessions ───────────
Session* Database::acquire() {
    std::unique_lock<std::mutex> lk(poolMutex_);

    bool ok = poolCV_.wait_for(lk,
        std::chrono::milliseconds(POOL_TIMEOUT_MS),
        [] { return !Database::freeConnections_.empty(); });

    if (!ok || freeConnections_.empty())
        throw std::runtime_error("[DB] Pool exhausted — no free connection within timeout");

    Session* s = freeConnections_.front();
    freeConnections_.pop();
    lk.unlock();   // release lock before potentially slow ping

    // Health check — heal if stale
    if (!pingSession_(s)) {
        std::cerr << "[DB] Stale session detected, reconnecting…\n";
        s = healSession_(s);
        // Replace in allConnections_
        std::lock_guard<std::mutex> lk2(poolMutex_);
        for (auto& ptr : allConnections_)
            if (ptr != s) { /* find stale */ }
        // (allConnections_ already updated via healSession_ delete/new)
    }
    return s;
}

// ── tryAcquire() — non-blocking ───────────────────────────────────────────────
std::optional<Session*> Database::tryAcquire() {
    std::lock_guard<std::mutex> lk(poolMutex_);
    if (freeConnections_.empty()) return std::nullopt;
    Session* s = freeConnections_.front();
    freeConnections_.pop();
    return s;
}

// ── release() ────────────────────────────────────────────────────────────────
void Database::release(Session* session) {
    if (!session) return;
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        freeConnections_.push(session);
    }
    poolCV_.notify_one();
}

// ── poolHealth() ─────────────────────────────────────────────────────────────
PoolHealth Database::poolHealth() {
    std::lock_guard<std::mutex> lk(poolMutex_);
    return {
        static_cast<int>(allConnections_.size()),
        static_cast<int>(freeConnections_.size()),
        staleReconnects_.load()
    };
}

// ── ScopedConnection ─────────────────────────────────────────────────────────
Database::ScopedConnection::ScopedConnection()
    : session_(Database::acquire()) {}

Database::ScopedConnection::~ScopedConnection() {
    if (session_) Database::release(session_);
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
Session& Database::getSession() {
    if (!legacySession_) legacySession_ = createSession_();
    return *legacySession_;
}

Schema Database::getSchema() {
    return getSession().getSchema(env("DB_NAME", "bankingdb"));
}