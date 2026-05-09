#pragma once
#include <string>
#include "Database.h"

// Dynamic Currency Conversion module
class CurrencyConverter {
public:
    static constexpr double BANK_FX_MARKUP_PERCENT = 2.0;

    struct ConversionResult {
        double originalAmount;
        std::string originalCurrency;
        double convertedAmount;
        std::string targetCurrency;
        double fxRate;
        double bankMarkupAmount;
    };

    // Converts an amount from source currency to AUD (our system's base currency).
    // Applies a 2% bank markup for cross-currency.
    static ConversionResult convertToBase(const std::string& sourceCurrency, double amount, mysqlx::Session& sess);
};
