#pragma once
#include "json.hpp"

// ── Card Lifecycle Management ─────────────────────────────────────────────────
// All functions receive the request JSON from the router and return a response JSON.

nlohmann::json processActivateCard(const nlohmann::json& data);
nlohmann::json processBlockCard(const nlohmann::json& data);
nlohmann::json processSetCardLimit(const nlohmann::json& data);
nlohmann::json processResetCardPIN(const nlohmann::json& data);
