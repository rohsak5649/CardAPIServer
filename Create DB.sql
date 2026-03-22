
/*
 * Core Banking Payment Switch – Database Schema Overview
 * Developer: Rohan Sakhare
 *
 * Database Design Summary:
 *
 * 1. Customer Layer:
 *    • accounts  → Stores customer account details and balances
 *    • cards     → Linked debit/credit cards mapped to accounts
 *    • currency  → Supported currency master table
 *
 * 2. Tokenization & Security:
 *    • ringpay_tokens → Secure wearable payment tokens with limits & expiry
 *
 * 3. Channel Transaction Tables:
 *    • transaction_atm     → ATM withdrawals & services
 *    • transaction_pos     → POS purchases & refunds
 *    • transaction_ecom    → E-commerce payments & refunds
 *    • transaction_mobile  → Mobile banking transfers & payments
 *    • transaction_qrcode  → QR-based merchant payments
 *    • transaction_ringpay → Wearable contactless payments
 *
 * 4. Risk & Fraud Monitoring:
 *    • transaction_falcon → Fraud detection logs & declined transactions
 *
 * 5. Central Transaction Registry:
 *    • transactions → Master table linking all channel transactions
 *
 * Architecture ensures:
 * • Channel isolation
 * • Secure tokenization
 * • Fraud monitoring
 * • Transaction traceability
 * • Scalable payment processing
 */
CREATE TABLE `accounts` (
                            `account_id` int NOT NULL AUTO_INCREMENT,
                            `account_number` varchar(20) NOT NULL,
                            `balance` decimal(12,2) DEFAULT '0.00',
                            `country_code` char(2) NOT NULL DEFAULT 'AU',
                            `is_frozen` tinyint(1) DEFAULT '0',
                            `currency_id` int NOT NULL,
                            PRIMARY KEY (`account_id`),
                            UNIQUE KEY `account_number` (`account_number`)
) ENGINE=InnoDB AUTO_INCREMENT=7 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci


CREATE TABLE `cards` (
                         `card_id` int NOT NULL AUTO_INCREMENT,
                         `pan` varchar(20) NOT NULL,
                         `scheme` varchar(20) DEFAULT NULL,
                         `card_type` varchar(20) DEFAULT NULL,
                         `expiry` varchar(10) NOT NULL,
                         `cvv` varchar(4) DEFAULT NULL,
                         `cardholder_name` varchar(100) NOT NULL,
                         `account_number` varchar(20) NOT NULL,
                         `status` enum('ACTIVE','BLOCKED') DEFAULT 'ACTIVE',
                         `card_priority` enum('PRIMARY','SECONDARY','TERTIARY') DEFAULT 'PRIMARY',
                         PRIMARY KEY (`card_id`),
                         UNIQUE KEY `pan` (`pan`)
) ENGINE=InnoDB AUTO_INCREMENT=17 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `currency` (
                            `currency_id` int NOT NULL AUTO_INCREMENT,
                            `currency_code` char(3) NOT NULL,
                            `currency_name` varchar(50) NOT NULL,
                            `symbol` varchar(10) DEFAULT NULL,
                            `is_base` tinyint(1) DEFAULT '0',
                            PRIMARY KEY (`currency_id`),
                            UNIQUE KEY `currency_code` (`currency_code`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `ringpay_tokens` (
                                  `token_id` bigint NOT NULL AUTO_INCREMENT,
                                  `token` varchar(128) DEFAULT NULL,
                                  `card_pan` varchar(20) DEFAULT NULL,
                                  `account_number` varchar(20) DEFAULT NULL,
                                  `daily_limit` decimal(10,2) DEFAULT '5000.00',
                                  `single_txn_limit` decimal(10,2) DEFAULT '2000.00',
                                  `created_date` date DEFAULT NULL,
                                  `status` varchar(20) DEFAULT 'ACTIVE',
                                  `expires_at` timestamp NULL DEFAULT NULL,
                                  PRIMARY KEY (`token_id`),
                                  UNIQUE KEY `token` (`token`)
) ENGINE=InnoDB AUTO_INCREMENT=9 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_atm` (
                                   `id` bigint NOT NULL AUTO_INCREMENT,
                                   `transaction_id` varchar(50) DEFAULT NULL,
                                   `client_txn_id` varchar(50) DEFAULT NULL,
                                   `atm_id` varchar(50) DEFAULT NULL,
                                   `terminal_id` varchar(50) DEFAULT NULL,
                                   `location` varchar(100) DEFAULT NULL,
                                   `account_number` varchar(30) DEFAULT NULL,
                                   `amount` decimal(12,2) DEFAULT NULL,
                                   `fee` decimal(10,2) DEFAULT NULL,
                                   `card_pan` varchar(20) DEFAULT NULL,
                                   `card_scheme` varchar(20) DEFAULT NULL,
                                   `status` enum('SUCCESS','FAILED') DEFAULT NULL,
                                   `message` varchar(255) DEFAULT NULL,
                                   `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                   PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_ecom` (
                                    `id` bigint NOT NULL AUTO_INCREMENT,
                                    `transaction_id` varchar(50) DEFAULT NULL,
                                    `client_txn_id` varchar(50) DEFAULT NULL,
                                    `merchant_id` varchar(50) DEFAULT NULL,
                                    `order_id` varchar(50) DEFAULT NULL,
                                    `currency` varchar(10) DEFAULT NULL,
                                    `transaction_scope` varchar(20) DEFAULT NULL,
                                    `account_number` varchar(30) DEFAULT NULL,
                                    `amount` decimal(12,2) DEFAULT NULL,
                                    `fee` decimal(10,2) DEFAULT NULL,
                                    `card_pan` varchar(20) DEFAULT NULL,
                                    `card_scheme` varchar(20) DEFAULT NULL,
                                    `status` enum('SUCCESS','FAILED') DEFAULT NULL,
                                    `message` varchar(255) DEFAULT NULL,
                                    `reference_txn_id` varchar(50) DEFAULT NULL,
                                    `refunded_amount` decimal(12,2) DEFAULT '0.00',
                                    `flag` varchar(5) DEFAULT 'N',
                                    `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                    PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=10 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_falcon` (
                                      `id` int NOT NULL AUTO_INCREMENT,
                                      `transaction_id` varchar(100) DEFAULT NULL,
                                      `client_txn_id` varchar(100) DEFAULT NULL,
                                      `device_id` varchar(100) DEFAULT NULL,
                                      `mobile_number` varchar(20) DEFAULT NULL,
                                      `account_number` varchar(50) DEFAULT NULL,
                                      `amount` decimal(12,2) DEFAULT NULL,
                                      `fraud_reason` varchar(255) DEFAULT NULL,
                                      `status` varchar(20) DEFAULT NULL,
                                      `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                      PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=42 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_mobile` (
                                      `id` bigint NOT NULL AUTO_INCREMENT,
                                      `transaction_id` varchar(50) DEFAULT NULL,
                                      `client_txn_id` varchar(50) DEFAULT NULL,
                                      `device_id` varchar(50) DEFAULT NULL,
                                      `mobile_number` varchar(20) DEFAULT NULL,
                                      `account_number` varchar(30) DEFAULT NULL,
                                      `amount` decimal(12,2) DEFAULT NULL,
                                      `fee` decimal(10,2) DEFAULT NULL,
                                      `status` enum('SUCCESS','FAILED') DEFAULT NULL,
                                      `message` varchar(255) DEFAULT NULL,
                                      `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                      PRIMARY KEY (`id`),
                                      KEY `idx_mobile_acc_time` (`account_number`,`created_at`)
) ENGINE=InnoDB AUTO_INCREMENT=5 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_pos` (
                                   `id` bigint NOT NULL AUTO_INCREMENT,
                                   `transaction_id` varchar(50) DEFAULT NULL,
                                   `client_txn_id` varchar(50) DEFAULT NULL,
                                   `original_purchase_id` bigint DEFAULT NULL,
                                   `merchant_id` varchar(50) DEFAULT NULL,
                                   `terminal_id` varchar(50) DEFAULT NULL,
                                   `location` varchar(100) DEFAULT NULL,
                                   `account_number` varchar(30) DEFAULT NULL,
                                   `amount` decimal(12,2) DEFAULT NULL,
                                   `fee` decimal(10,2) DEFAULT NULL,
                                   `card_pan` varchar(20) DEFAULT NULL,
                                   `card_scheme` varchar(20) DEFAULT NULL,
                                   `status` enum('SUCCESS','FAILED') DEFAULT NULL,
                                   `message` varchar(255) DEFAULT NULL,
                                   `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                   `refunded_amount` double DEFAULT '0',
                                   `flag` varchar(2) DEFAULT 'N',
                                   PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=6 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_qrcode` (
                                      `id` bigint NOT NULL AUTO_INCREMENT,
                                      `transaction_id` varchar(50) DEFAULT NULL,
                                      `client_txn_id` varchar(50) DEFAULT NULL,
                                      `qr_raw_data` text,
                                      `merchant_name` varchar(100) DEFAULT NULL,
                                      `merchant_id` varchar(50) DEFAULT NULL,
                                      `terminal_id` varchar(50) DEFAULT NULL,
                                      `account_number` varchar(30) DEFAULT NULL,
                                      `amount` decimal(12,2) DEFAULT NULL,
                                      `fee` decimal(10,2) DEFAULT NULL,
                                      `currency` varchar(10) DEFAULT NULL,
                                      `transaction_scope` varchar(20) DEFAULT NULL,
                                      `status` enum('SUCCESS','FAILED') DEFAULT NULL,
                                      `message` varchar(255) DEFAULT NULL,
                                      `orig_ref_id` bigint DEFAULT NULL,
                                      `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                      PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=3 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci


CREATE TABLE `transaction_ringpay` (
                                       `id` bigint NOT NULL AUTO_INCREMENT,
                                       `transaction_id` varchar(64) DEFAULT NULL,
                                       `token` varchar(128) DEFAULT NULL,
                                       `account_number` varchar(20) DEFAULT NULL,
                                       `amount` decimal(10,2) DEFAULT NULL,
                                       `fee` decimal(10,2) DEFAULT NULL,
                                       `status` varchar(20) DEFAULT NULL,
                                       `message` varchar(255) DEFAULT NULL,
                                       `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                       `reversal_status` varchar(20) DEFAULT 'NA',
                                       `device_id` varchar(50) DEFAULT NULL,
                                       `ip_address` varchar(50) DEFAULT NULL,
                                       `merchant_id` varchar(30) DEFAULT NULL,
                                       PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=50 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transactions` (
                                `id` bigint NOT NULL AUTO_INCREMENT,
                                `table_name` varchar(50) NOT NULL,
                                `reference_id` bigint NOT NULL,
                                `status` varchar(20) DEFAULT NULL,
                                `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=190 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
