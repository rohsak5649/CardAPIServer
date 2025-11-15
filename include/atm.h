#ifndef ATM_H
#define ATM_H

#include "json.hpp"
using json = nlohmann::json;

// Declare the function implemented in atm.cpp
json processATMTransaction(const json& data);

#endif
