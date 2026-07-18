cmake_minimum_required(VERSION 3.15)
project(BankingAPIServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Platform-specific dependency paths ────────────────────────────────────────
if(WIN32)
    # Windows — MySQL Connector C++ installed to default path by MSI installer
    set(MYSQL_CONNECTOR_DIR "C:/Program Files/MySQL/MySQL Connector C++ 9.5")

    include_directories(
            ${PROJECT_SOURCE_DIR}/include
            "${MYSQL_CONNECTOR_DIR}/include"
            "${MYSQL_CONNECTOR_DIR}/include/mysqlx"
    )

    link_directories(
            "${MYSQL_CONNECTOR_DIR}/lib64/vs14"
            "${MYSQL_CONNECTOR_DIR}/lib64"
    )

    # OpenSSL — installed by Chocolatey to this path on windows-latest runners
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")

else()
    # macOS (Homebrew) / Linux
    include_directories(
            ${PROJECT_SOURCE_DIR}/include
            /opt/homebrew/include
            /opt/homebrew/Cellar/mysql-connector-c++/9.5.0/include
    )

    link_directories(
            /opt/homebrew/lib
            /opt/homebrew/Cellar/mysql-connector-c++/9.5.0/lib
    )

    # OpenSSL — Homebrew on macOS
    if(APPLE)
        set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
    endif()

endif()

# ── OpenSSL ────────────────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)

# ── Threads (REQUIRED for multithreading) ─────────────────────────────────────
find_package(Threads REQUIRED)

# ── Source files ───────────────────────────────────────────────────────────────
add_executable(BankingAPIServer
        "parser&router.cpp"
        atm.cpp
        mobile.cpp
        include/atm.h
        include/mobile.h
        pos.cpp
        include/pos.h
        iccw.cpp
        include/iccw.h
        issue.cpp
        include/issue.h
        include/ecom.h
        ecom.cpp
        include/qrcode.h
        qrcode.cpp
        ringpay.cpp
        include/ringpay.h
        include/TransactionLogger.h
        TransactionLogger.cpp
        include/account.h
        account.cpp
        include/falcon.h
        falcon.cpp
        include/Database.h
        Database.cpp
        include/pin.h
        pin.cpp
        include/panencrypted.h
        panencrypted.cpp
        reversal.cpp
        include/reversal.h
        idempotency.cpp
        include/idempotency.h
        currency_converter.cpp
        include/currency_converter.h
        accounting.cpp
        include/accounting.h
        auth.cpp
        include/auth.h
        card_mgmt.cpp
        include/card_mgmt.h
        tds.cpp
        include/tds.h
        include/global_contant.h
        DatabaseQueries.cpp
        include/DatabaseQueries.h
        DbConfig.cpp
        include/DbConfig.h
)

# ── Compiler flags ─────────────────────────────────────────────────────────────
# CPPHTTPLIB_THREAD_POOL_COUNT is respected by httplib when using ThreadPool.
target_compile_definitions(BankingAPIServer PRIVATE
        CPPHTTPLIB_THREAD_POOL_COUNT=100
)

# ── Link libraries ─────────────────────────────────────────────────────────────
target_link_libraries(BankingAPIServer
        mysqlcppconnx
        OpenSSL::Crypto
        Threads::Threads       # ← adds -pthread on Linux/macOS
)


# ── Build type flags ───────────────────────────────────────────────────────────
# Do NOT hardcode CMAKE_BUILD_TYPE here — let CLion's Debug/Release toolbar
# control it so breakpoints and debug symbols work correctly in Debug mode.
#
# Debug   → -O0 -g3  (no optimisation, full DWARF symbols → breakpoints work)
# Release → -O2      (optimised, no debug info → fast production binary)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# GCC / Clang flags (macOS and Linux) — MSVC uses its own /Od /O2 defaults
if(NOT MSVC)
    target_compile_options(BankingAPIServer PRIVATE
        $<$<CONFIG:Debug>:   -O0 -g3 -DDEBUG>
        $<$<CONFIG:Release>: -O2 -DNDEBUG>
    )
endif()
