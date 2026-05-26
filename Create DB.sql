
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
 * 6. Reversal Engine:
 *    • transaction_reversal → Per-reversal detail rows written when DB is UP.
 *                             Linked to the originating channel table via
 *                             original_table_name + original_reference_id and
 *                             to the master registry via transactions.
 *    • reversal_drop_file  → Offline queue. Written when the DB is DOWN at
 *                             reversal time. A background retry worker polls
 *                             PENDING rows and replays them via processReversal()
 *                             once the DB recovers.
 *
 * NOTE: channel tables that carry `reversal_status` column track whether a
 *       particular debit row has been reversed.  See ALTER TABLE statements
 *       at the bottom of this file.
 *
 * Architecture ensures:
 * • Channel isolation
 * • Secure tokenization
 * • Fraud monitoring
 * • Transaction traceability
 * • Scalable payment processing
 * • Automatic debit reversal with DB-down resilience
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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci


CREATE TABLE `cards` (
                         `card_id` int NOT NULL AUTO_INCREMENT,
                         `pan` varchar(20) NOT NULL,
                         `masked_pan` varchar(20) DEFAULT NULL,
                         `encrypted_pan` varchar(255) DEFAULT NULL,
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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `currency` (
                            `currency_id` int NOT NULL AUTO_INCREMENT,
                            `currency_code` char(3) NOT NULL,
                            `currency_name` varchar(50) NOT NULL,
                            `symbol` varchar(10) DEFAULT NULL,
                            `is_base` tinyint(1) DEFAULT '0',
                            PRIMARY KEY (`currency_id`),
                            UNIQUE KEY `currency_code` (`currency_code`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transaction_iccw` (
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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci


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
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

CREATE TABLE `transactions` (
                                `id` bigint NOT NULL AUTO_INCREMENT,
                                `table_name` varchar(50) NOT NULL,
                                `reference_id` bigint NOT NULL,
                                `status` varchar(20) DEFAULT NULL,
                                `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
                                PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci

-- ─────────────────────────────────────────────────────────────────────────────
-- 6. Reversal Tables
--    • transaction_reversal  — per-reversal detail record (DB is UP path)
--    • reversal_drop_file    — offline queue when DB is DOWN (retry later)
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE `transaction_reversal` (
    `id`                       bigint        NOT NULL AUTO_INCREMENT,
    `reversal_transaction_id`  varchar(60)   NOT NULL COMMENT 'Generated reversal TxnId (rev-…)',
    `client_txn_id`            varchar(60)   DEFAULT NULL,
    `original_transaction_id`  varchar(60)   NOT NULL COMMENT 'TxnId of the original debit',
    `original_table_name`      varchar(50)   NOT NULL COMMENT 'Channel table of the original txn e.g. transaction_atm',
    `original_reference_id`    bigint        NOT NULL COMMENT 'Auto-increment id in the original channel table',
    `channel`                  varchar(20)   NOT NULL COMMENT 'Originating channel: ATM / POS / MOBILE / ECOM …',
    `account_number`           varchar(30)   NOT NULL,
    `amount`                   decimal(12,2) NOT NULL COMMENT 'Original deducted amount being reversed',
    `fee`                      decimal(10,2) NOT NULL DEFAULT '0.00' COMMENT 'Original fee being reversed',
    `card_pan`                 varchar(20)   DEFAULT NULL,
    `card_scheme`              varchar(20)   DEFAULT NULL,
    `reason`                   varchar(100)  NOT NULL DEFAULT 'TIMEOUT' COMMENT 'TIMEOUT / NETWORK_ERROR / MANUAL …',
    `status`                   varchar(20)   NOT NULL DEFAULT 'REVERSED',
    `message`                  varchar(255)  DEFAULT NULL,
    `created_at`               timestamp     NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uq_reversal_txn_id` (`reversal_transaction_id`),
    KEY `idx_rev_original_txn`      (`original_transaction_id`),
    KEY `idx_rev_account`           (`account_number`),
    KEY `idx_rev_original_ref`      (`original_table_name`, `original_reference_id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Reversal records for transactions where money was deducted but response was not received';

-- ─────────────────────────────────────────────────────────────────────────────
-- reversal_drop_file — offline queue written when DB is unreachable at the
-- time of reversal.  A background worker polls PENDING rows and replays them
-- by calling processReversal() once the DB recovers.
-- ─────────────────────────────────────────────────────────────────────────────

CREATE TABLE `reversal_drop_file` (
    `id`                       bigint        NOT NULL AUTO_INCREMENT,
    `reversal_id`              varchar(60)   NOT NULL COMMENT 'Pre-generated reversal ID',
    `original_transaction_id`  varchar(60)   NOT NULL,
    `original_table_name`      varchar(50)   NOT NULL,
    `original_reference_id`    bigint        NOT NULL,
    `channel`                  varchar(20)   NOT NULL,
    `account_number`           varchar(30)   NOT NULL,
    `amount`                   decimal(12,2) NOT NULL,
    `fee`                      decimal(10,2) NOT NULL DEFAULT '0.00',
    `reason`                   varchar(100)  NOT NULL DEFAULT 'TIMEOUT',
    `raw_payload`              mediumtext    DEFAULT NULL COMMENT 'Full JSON payload for replay',
    `retry_status`             varchar(20)   NOT NULL DEFAULT 'PENDING'
                                             COMMENT 'PENDING | RETRYING | REPLAYED | FAILED',
    `retry_count`              int           NOT NULL DEFAULT '0',
    `last_retry_at`            timestamp     NULL DEFAULT NULL,
    `replayed_reversal_id`     varchar(60)   DEFAULT NULL COMMENT 'reversal_transaction_id after successful replay',
    `created_at`               timestamp     NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at`               timestamp     NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uq_drop_reversal_id`    (`reversal_id`),
    KEY `idx_drop_retry_status`         (`retry_status`),
    KEY `idx_drop_account`              (`account_number`),
    KEY `idx_drop_original_txn`         (`original_transaction_id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
  COMMENT='Offline queue for reversals that could not be committed when the DB was down';

-- ─────────────────────────────────────────────────────────────────────────────
-- ALTER existing channel tables to add reversal_status column.
-- Uses a stored procedure + INFORMATION_SCHEMA check because
-- "ADD COLUMN IF NOT EXISTS" requires MySQL 8.0.3+ and may not be available
-- on all patch levels.  This approach is safe on any MySQL 8.0.x build.
-- ─────────────────────────────────────────────────────────────────────────────

DROP PROCEDURE IF EXISTS _add_reversal_status;

DELIMITER $$
CREATE PROCEDURE _add_reversal_status(IN tbl VARCHAR(64))
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM   INFORMATION_SCHEMA.COLUMNS
        WHERE  TABLE_SCHEMA = DATABASE()
          AND  TABLE_NAME   = tbl
          AND  COLUMN_NAME  = 'reversal_status'
    ) THEN
        SET @sql = CONCAT(
            'ALTER TABLE `', tbl, '` ',
            'ADD COLUMN `reversal_status` varchar(20) NOT NULL DEFAULT ''NA'' ',
            'COMMENT ''NA | REVERSED'' AFTER `message`'
        );
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;
END$$
DELIMITER ;

-- transaction_ringpay already has reversal_status — excluded from list below.
CALL _add_reversal_status('transaction_atm');
CALL _add_reversal_status('transaction_pos');
CALL _add_reversal_status('transaction_mobile');
CALL _add_reversal_status('transaction_ecom');
CALL _add_reversal_status('transaction_qrcode');
CALL _add_reversal_status('transaction_iccw');

DROP PROCEDURE IF EXISTS _add_reversal_status;

-- ─────────────────────────────────────────────────────────────────────────────
-- 8. Idempotency Keys (Prevent Double Charging)
-- ─────────────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS `idempotency_keys` (
    `idempotency_key` varchar(100) NOT NULL,
    `request_path` varchar(100) NOT NULL,
    `status` varchar(20) NOT NULL COMMENT 'IN_PROGRESS, COMPLETED, FAILED',
    `response_code` int DEFAULT NULL,
    `response_body` json DEFAULT NULL,
    `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`idempotency_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- ─────────────────────────────────────────────────────────────────────────────
-- 9. Double Entry Ledger System
-- ─────────────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS `ledger_accounts` (
    `ledger_id` int NOT NULL AUTO_INCREMENT,
    `account_number` varchar(30) DEFAULT NULL,
    `account_name` varchar(100) NOT NULL,
    `account_type` varchar(20) NOT NULL,
    `balance` decimal(15,2) DEFAULT '0.00',
    `currency` char(3) NOT NULL DEFAULT 'AUD',
    `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`ledger_id`),
    UNIQUE KEY `uq_ledger_account` (`account_number`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS `ledger_entries` (
    `entry_id` bigint NOT NULL AUTO_INCREMENT,
    `transaction_id` varchar(64) NOT NULL,
    `ledger_id` int NOT NULL,
    `amount` decimal(15,2) NOT NULL COMMENT 'Positive for Credit, Negative for Debit',
    `entry_type` varchar(10) NOT NULL COMMENT 'DEBIT or CREDIT',
    `description` varchar(255) DEFAULT NULL,
    `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`entry_id`),
    KEY `idx_ledger_txn` (`transaction_id`),
    KEY `idx_ledger_acc` (`ledger_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- ─────────────────────────────────────────────────────────────────────────────
-- 10. Dynamic Currency Conversion (FX Rates)
-- ─────────────────────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS `exchange_rates` (
    `base_currency` char(3) NOT NULL,
    `target_currency` char(3) NOT NULL,
    `rate` decimal(10,6) NOT NULL,
    `updated_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`base_currency`, `target_currency`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- Insert Mock Exchange Rates (Base Currency to AUD)
INSERT IGNORE INTO exchange_rates (base_currency, target_currency, rate) VALUES 
('USD', 'AUD', 1.520000),
('EUR', 'AUD', 1.650000),
('GBP', 'AUD', 1.900000),
('JPY', 'AUD', 0.010000),
('NZD', 'AUD', 0.920000),
('INR', 'AUD', 0.018000),
('AUD', 'USD', 0.657894);

-- Insert Internal Bank Ledger Accounts
INSERT IGNORE INTO ledger_accounts (account_number, account_name, account_type, currency) VALUES 
('BANK_FEE_REV', 'Bank Fee Revenue', 'REVENUE', 'AUD'),
('BANK_FX_REV', 'Bank FX Revenue', 'REVENUE', 'AUD'),
('BANK_MERCHANT_SETT', 'Merchant Settlement Suspense', 'LIABILITY', 'AUD');
