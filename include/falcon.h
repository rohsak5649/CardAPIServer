#ifndef FALCON_H
#define FALCON_H

#include <string>
#include <mysqlx/xdevapi.h>

using namespace mysqlx;

class Falcon {
public:
    Falcon(Session& session);

    bool checkFraud(const std::string& accountNumber,
                    double amount,
                    std::string& reason);

    void logFraud(const std::string& txnId,
                  const std::string& clientTxnId,
                  const std::string& deviceId,
                  const std::string& mobileNo,
                  const std::string& accountNumber,
                  double amount,
                  const std::string& reason);

private:
    Session& sess;

    // EXISTING (MOBILE)
    bool checkSameSecond(const std::string& accountNumber);
    bool checkVelocity(const std::string& accountNumber);

    // ✅ NEW (ECOM)
    bool checkSameSecondForEcom(const std::string& accountNumber);
    bool checkVelocityForEcom(const std::string& accountNumber);
};

#endif