/*
 * Copyright (c) Rohan Sakhare — All rights reserved.
 *
 * DATABASE CONNECTION POOL  v3.1  (C++20 · Thread-Safe · Self-Healing)
 * ─────────────────────────────────────────────────────────────────────
 * WHAT'S NEW IN v3.1
 *   ✅ POOL_SIZE now read from DB_POOL_SIZE env-var at runtime (fallback: 10)
 *      — was hard-coded 30, which exceeded mysqlx_max_connections on many
 *        dev/staging MySQL installs and caused "EOS state" CDK errors
 *   ✅ initPool() is now resilient: retries each slot up to
 *      POOL_CONNECT_RETRIES times with POOL_CONNECT_DELAY_MS back-off
 *      before failing; partial pool still starts if ≥ 1 connection succeeds
 *   ✅ initPool() only throws when ZERO connections could be established
 *   ✅ Pool size capped at runtime by DB_POOL_SIZE (override via env)
 *   ✅ initPool() logs a warning if fewer connections than requested succeed
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

#include "DbConfig.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <mysqlx/xdevapi.h>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace mysqlx;

// ── Compile-time tuneable constants ──────────────────────────────────────────
// POOL_SIZE is the *maximum* desired pool size.  The actual pool will be
// smaller if MySQL rejects connections (e.g. mysqlx_max_connections limit).
// Override at runtime via DB_POOL_SIZE environment variable.
inline constexpr int POOL_SIZE_DEFAULT =
    10;                                  // safe default for most MySQL installs
inline constexpr int POOL_SIZE_MAX = 50; // absolute ceiling
inline constexpr int POOL_TIMEOUT_MS = 5'000; // ms to wait for a free slot
inline constexpr int HEALTH_CHECK_RETRIES =
    3; // reconnect attempts before giving up
inline constexpr int POOL_CONNECT_RETRIES =
    3; // retries per slot during initPool()
inline constexpr int POOL_CONNECT_DELAY_MS =
    200; // back-off between slot retries (ms)
// ─────────────────────────────────────────────────────────────────────────────

// Kept for binary-compat with code that references POOL_SIZE directly.
// Prefer Database::poolSize() which returns the runtime value.
inline constexpr int POOL_SIZE = POOL_SIZE_DEFAULT;

struct PoolHealth {
  int total;
  int free;
  int staleReconnects; // cumulative reconnects since start
};

class Database {
public:
  // ── RAII scoped connection ─────────────────────────────────────────────
  class ScopedConnection {
  public:
    ScopedConnection();
    ~ScopedConnection();

    ScopedConnection(const ScopedConnection &) = delete;
    ScopedConnection &operator=(const ScopedConnection &) = delete;
    ScopedConnection(ScopedConnection &&) noexcept;
    ScopedConnection &operator=(ScopedConnection &&) noexcept;

    Session *operator->() const { return session_; }
    Session &operator*() const { return *session_; }
    [[nodiscard]] bool valid() const noexcept { return session_ != nullptr; }

  private:
    Session *session_ = nullptr;
  };

  // ── Pool interface ─────────────────────────────────────────────────────
  [[nodiscard]] static Session *acquire();
  static void release(Session *session);
  static std::optional<Session *> tryAcquire(); // non-blocking

  // ── Pool management ────────────────────────────────────────────────────
  static void initPool();
  static void destroyPool();
  static PoolHealth poolHealth();

  // Returns the *actual* pool size determined at runtime from DB_POOL_SIZE
  // env-var (clamped to [1, POOL_SIZE_MAX]).  Use this instead of POOL_SIZE.
  static int poolSize() noexcept { return poolSizeRuntime_; }

  // ── Legacy shim (NOT thread-safe — migrate to ScopedConnection) ────────
  static Session &getSession();
  static Schema getSchema();

private:
  static Session *createSession_();
  static bool pingSession_(Session *s) noexcept;
  static Session *healSession_(Session *s);

  // Resolved once at pool init from DB_POOL_SIZE env-var
  static int poolSizeRuntime_;

  // ── Encrypted config credentials (loaded once at initPool()) ────────────
  // Path to the config file — relative to the working directory of the binary.
  static constexpr const char *DB_CONFIG_PATH = "db.ini";
  static DbCredentials creds_; // populated by DbConfig::load()

  static std::vector<Session *> allConnections_;
  static std::queue<Session *> freeConnections_;
  static std::mutex poolMutex_;
  static std::condition_variable poolCV_;
  static bool poolReady_;
  static std::atomic<int> staleReconnects_;

  static Session *legacySession_;
};