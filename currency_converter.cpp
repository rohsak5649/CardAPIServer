#include "currency_converter.h"
#include "DatabaseQueries.h"
#include <stdexcept>
#include <iostream>
#include <cmath>

using namespace mysqlx;

CurrencyConverter::ConversionResult CurrencyConverter::convertToBase(const std::string& sourceCurrency, double amount, Session& sess) {
    ConversionResult result;
    result.originalAmount = amount;
    result.originalCurrency = sourceCurrency;
    result.targetCurrency = "AUD";
    
    if (sourceCurrency == "AUD") {
        result.convertedAmount = amount;
        result.fxRate = 1.0;
        result.bankMarkupAmount = 0.0;
        return result;
    }

    // Lookup FX rate
    auto rateOpt = DatabaseQueries::getExchangeRate(sess, sourceCurrency);
    if (!rateOpt) {
        throw std::runtime_error("ERR_FX_RATE_NOT_FOUND");
    }
    double rate = *rateOpt;

    double convertedRaw = amount * rate;
    double markup = convertedRaw * (BANK_FX_MARKUP_PERCENT / 100.0);
    
    result.fxRate = rate;
    result.bankMarkupAmount = std::round(markup * 100.0) / 100.0;
    result.convertedAmount = std::round((convertedRaw + result.bankMarkupAmount) * 100.0) / 100.0;

    return result;
}
