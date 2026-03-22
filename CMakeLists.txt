cmake_minimum_required(VERSION 3.15)
project(BankingAPIServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ✅ Include directories
include_directories(
        ${PROJECT_SOURCE_DIR}/include
        /opt/homebrew/include
        /opt/homebrew/Cellar/mysql-connector-c++/9.5.0/include
)

# ✅ Link directories
link_directories(
        /opt/homebrew/lib
        /opt/homebrew/Cellar/mysql-connector-c++/9.5.0/lib
)
set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
find_package(OpenSSL REQUIRED)
# ✅ Add your source files
add_executable(BankingAPIServer
        parser&router.cpp
        atm.cpp
        mobile.cpp
        include/atm.h
        include/mobile.h
        pos.cpp
        include/pos.h
        issue.cpp
        include/issue.h
        include/ecom.h
        ecom.cpp
        include/qrcode.h
        qrcode.cpp
        ringpay.cpp
        include/ringpay.h
        include/falcon.h
        falcon.cpp
        include/Database.h
        Database.cpp
        include/pin.h
        pin.cpp
        include/panencrypted.h
        panencrypted.cpp
)

# ✅ Link the correct MySQL library (X DevAPI)
target_link_libraries(BankingAPIServer
        mysqlcppconnx
        OpenSSL::Crypto
)

set(CMAKE_BUILD_TYPE Debug)
