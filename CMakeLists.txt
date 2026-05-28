cmake_minimum_required(VERSION 3.15)

project(BankingAPIServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include folders
include_directories(
    ${PROJECT_SOURCE_DIR}/include
)

# OpenSSL
find_package(OpenSSL REQUIRED)

# Threads
find_package(Threads REQUIRED)

# Executable
add_executable(BankingAPIServer

    "parser&router.cpp"

    atm.cpp
    mobile.cpp
    pos.cpp
    iccw.cpp
    issue.cpp
    ecom.cpp
    qrcode.cpp
    ringpay.cpp
    TransactionLogger.cpp
    account.cpp
    falcon.cpp
    Database.cpp
    pin.cpp
    panencrypted.cpp
    reversal.cpp
    idempotency.cpp
    currency_converter.cpp
    accounting.cpp
    auth.cpp
    card_mgmt.cpp
    tds.cpp
)

# Thread pool
target_compile_definitions(BankingAPIServer PRIVATE
    CPPHTTPLIB_THREAD_POOL_COUNT=100
)

# Link libraries
target_link_libraries(BankingAPIServer
    OpenSSL::Crypto
    Threads::Threads
)

# Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

target_compile_options(BankingAPIServer PRIVATE
    $<$<CONFIG:Debug>: -O0 -g3 -DDEBUG>
    $<$<CONFIG:Release>: -O2 -DNDEBUG>
)
