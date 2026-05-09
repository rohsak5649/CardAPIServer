#pragma once
#include "json.hpp"

// ── 3D Secure (3DS) Mock Flow ─────────────────────────────────────────────────
// Step 1: processECOM3DSInitiate() — called for 3DS ECOM PURCHASE
//         Returns { challengeId, otpHint (for testing), redirectUrl }
// Step 2: process3DSVerify()       — called at POST /3ds/verify
//         Accepts { challengeId, otp } → completes the charge
nlohmann::json processECOM3DSInitiate(const nlohmann::json& data);
nlohmann::json process3DSVerify(const nlohmann::json& data);
