#pragma once
#include <string>
#include "Database.h"

class Accounting {
public:
    static void processPurchaseLedger(const std::string& transactionId, 
                                    const std::string& customerAccount,
                                    const std::string& merchantAccount,
                                    double totalAmount,
                                    double bankFee,
                                    double fxMarkup,
                                    const std::string& description,
                                    mysqlx::Session& sess);
                                    
    static void processRefundLedger(const std::string& transactionId,
                                    const std::string& customerAccount,
                                    const std::string& merchantAccount,
                                    double refundAmount,
                                    const std::string& description,
                                    mysqlx::Session& sess);
};
