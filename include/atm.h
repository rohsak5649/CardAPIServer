#ifndef ATM_H
#define ATM_H

#include "json.hpp"
using json = nlohmann::json;

// Declare the function implemented in atm.cpp
json processATMTransaction(const json& data);

// Called by the signal handler on SIGINT/SIGTERM so detached reversal threads
// skip DB access after the connection pool has been destroyed.
void setATMEngineShutdown() noexcept;

#endif
