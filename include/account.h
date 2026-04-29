#ifndef ACCOUNT_H
#define ACCOUNT_H

#include "json.hpp"

nlohmann::json processAddAccount(const nlohmann::json& data);
nlohmann::json processGetAccount(const nlohmann::json& data);
nlohmann::json processFreezeAccount(const nlohmann::json& data);
nlohmann::json processUnfreezeAccount(const nlohmann::json& data);
nlohmann::json processListAccounts(const nlohmann::json& data);

#endif
