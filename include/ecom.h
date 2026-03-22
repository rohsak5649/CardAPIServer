#ifndef ECOM_H
#define ECOM_H

#include "json.hpp"

using json = nlohmann::json;

// Wrapper (with timeout like mobile)
json processECOMTransaction(const json &data);

// Core logic (internal)
json processECOMTransactionCore(const json &data);

#endif