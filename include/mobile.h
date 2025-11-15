#ifndef MOBILE_H
#define MOBILE_H

#include "json.hpp"
using json = nlohmann::json;

json processMobileTransaction(const json &data);

#endif
