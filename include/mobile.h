#ifndef MOBILE_H
#define MOBILE_H

#include "json.hpp"
using json = nlohmann::json;
std::string generateMobileTxnId();
json processMobileTransaction(const json &data);

#endif
