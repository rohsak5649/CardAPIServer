#pragma once
#include <string>
#include <optional>
#include "json.hpp"
#include "Database.h"

using json = nlohmann::json;

struct IdempotencyRecord {
    std::string idempotency_key;
    std::string request_path;
    std::string status;
    int response_code;
    json response_body;
};

class Idempotency {
public:
    // Tries to acquire the idempotency key.
    // Returns std::nullopt if this is a new key (meaning we can proceed).
    // Returns a populated IdempotencyRecord if the key already exists.
    // Throws if the key is currently IN_PROGRESS.
    static std::optional<IdempotencyRecord> checkOrRegister(const std::string& key, const std::string& path);

    // Updates the idempotency record with the final response.
    static void update(const std::string& key, int response_code, const json& response_body);
};
