#ifndef RINGPAY_H
#define RINGPAY_H

#include "json.hpp"
using json = nlohmann::json;

// Declare the function implemented in ringpay.cpp
json processRingPayTransaction(const json& data);

#endif
