// qrcode.h — QR Code Payments Header
#ifndef QRCODE_H
#define QRCODE_H

#include "json.hpp"

// Forward declaration for JSON alias
using json = nlohmann::json;

// QR code payment processor function
json processQRCodePayment(const json &data);

#endif // QRCODE_H
