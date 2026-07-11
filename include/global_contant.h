#pragma once

#include <string>
#include <string_view>

enum class TransactionType {
    UNKNOWN,
    PURCHASE,
    REFUND,
    WITHDRAWAL,
    DEPOSIT,
    FUND_TRANSFER
};

inline TransactionType stringToTransactionType(std::string_view str) {
    if (str == "PURCHASE") return TransactionType::PURCHASE;
    if (str == "REFUND") return TransactionType::REFUND;
    if (str == "WITHDRAWAL" || str == "CASH_WITHDRAWAL") return TransactionType::WITHDRAWAL;
    if (str == "DEPOSIT") return TransactionType::DEPOSIT;
    if (str == "FUND_TRANSFER") return TransactionType::FUND_TRANSFER;
    return TransactionType::UNKNOWN;
}

inline std::string transactionTypeToString(TransactionType type) {
    switch (type) {
        case TransactionType::PURCHASE: return "PURCHASE";
        case TransactionType::REFUND: return "REFUND";
        case TransactionType::WITHDRAWAL: return "WITHDRAWAL";
        case TransactionType::DEPOSIT: return "DEPOSIT";
        case TransactionType::FUND_TRANSFER: return "FUND_TRANSFER";
        default: return "UNKNOWN";
    }
}

enum class ChannelType {
    UNKNOWN,
    ATM,
    MOBILE,
    POS,
    ICCW,
    ISSUER,
    ECOM,
    THREEDS_INITIATE,
    QRCODE,
    RINGPAY,
    CARD_DETAILS,
    CARD_ACTIVATE,
    CARD_BLOCK,
    CARD_SET_LIMIT,
    CARD_RESET_PIN,
    ADD_ACCOUNT,
    ACCOUNT_DETAILS,
    FREEZE_ACCOUNT,
    UNFREEZE_ACCOUNT,
    LIST_ACCOUNTS,
    REVERSAL
};

inline ChannelType stringToChannelType(std::string_view str) {
    if (str == "ATM") return ChannelType::ATM;
    if (str == "MOBILE") return ChannelType::MOBILE;
    if (str == "POS") return ChannelType::POS;
    if (str == "ICCW") return ChannelType::ICCW;
    if (str == "ISSUER") return ChannelType::ISSUER;
    if (str == "ECOM") return ChannelType::ECOM;
    if (str == "3DS_INITIATE") return ChannelType::THREEDS_INITIATE;
    if (str == "QRCODE") return ChannelType::QRCODE;
    if (str == "RINGPAY") return ChannelType::RINGPAY;
    if (str == "CARD_DETAILS") return ChannelType::CARD_DETAILS;
    if (str == "CARD_ACTIVATE") return ChannelType::CARD_ACTIVATE;
    if (str == "CARD_BLOCK") return ChannelType::CARD_BLOCK;
    if (str == "CARD_SET_LIMIT") return ChannelType::CARD_SET_LIMIT;
    if (str == "CARD_RESET_PIN") return ChannelType::CARD_RESET_PIN;
    if (str == "ADD_ACCOUNT") return ChannelType::ADD_ACCOUNT;
    if (str == "ACCOUNT_DETAILS") return ChannelType::ACCOUNT_DETAILS;
    if (str == "FREEZE_ACCOUNT") return ChannelType::FREEZE_ACCOUNT;
    if (str == "UNFREEZE_ACCOUNT") return ChannelType::UNFREEZE_ACCOUNT;
    if (str == "LIST_ACCOUNTS") return ChannelType::LIST_ACCOUNTS;
    if (str == "REVERSAL") return ChannelType::REVERSAL;
    return ChannelType::UNKNOWN;
}

inline std::string channelTypeToString(ChannelType type) {
    switch (type) {
        case ChannelType::ATM: return "ATM";
        case ChannelType::MOBILE: return "MOBILE";
        case ChannelType::POS: return "POS";
        case ChannelType::ICCW: return "ICCW";
        case ChannelType::ISSUER: return "ISSUER";
        case ChannelType::ECOM: return "ECOM";
        case ChannelType::THREEDS_INITIATE: return "3DS_INITIATE";
        case ChannelType::QRCODE: return "QRCODE";
        case ChannelType::RINGPAY: return "RINGPAY";
        case ChannelType::CARD_DETAILS: return "CARD_DETAILS";
        case ChannelType::CARD_ACTIVATE: return "CARD_ACTIVATE";
        case ChannelType::CARD_BLOCK: return "CARD_BLOCK";
        case ChannelType::CARD_SET_LIMIT: return "CARD_SET_LIMIT";
        case ChannelType::CARD_RESET_PIN: return "CARD_RESET_PIN";
        case ChannelType::ADD_ACCOUNT: return "ADD_ACCOUNT";
        case ChannelType::ACCOUNT_DETAILS: return "ACCOUNT_DETAILS";
        case ChannelType::FREEZE_ACCOUNT: return "FREEZE_ACCOUNT";
        case ChannelType::UNFREEZE_ACCOUNT: return "UNFREEZE_ACCOUNT";
        case ChannelType::LIST_ACCOUNTS: return "LIST_ACCOUNTS";
        case ChannelType::REVERSAL: return "REVERSAL";
        default: return "UNKNOWN";
    }
}
