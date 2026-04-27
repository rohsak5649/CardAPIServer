#ifndef ISSUE_H
#define ISSUE_H

#include "json.hpp"
nlohmann::json processIssueCard(const nlohmann::json& data);
nlohmann::json processGetCardDetails(const nlohmann::json& data);

#endif
