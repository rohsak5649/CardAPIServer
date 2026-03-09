# CardAPIServer
# 🏦 Core Banking Payment Switch

**Developer:** Rohan Sakhare
**Project Type:** Multi-Channel Banking Transaction Switch
**Architecture:** Modular C++ Service with MySQL Database Backend

---

## 📌 Project Overview

The **Core Banking Payment Switch** is a centralized transaction processing system that enables secure, real-time financial transactions across multiple banking channels.

It simulates production-grade payment switch architecture used by banks and payment processors for handling ATM withdrawals, POS purchases, mobile banking, QR payments, e-commerce, and wearable payments.

The system ensures:

✔ Channel isolation
✔ Secure tokenization
✔ Fraud detection & monitoring
✔ Centralized transaction traceability
✔ Scalable payment processing

---

## 🧩 System Architecture

```
Client Channels
   ↓
API Parser & Router
   ↓
Channel Transaction Processor
   ↓
Fraud Engine (Falcon)
   ↓
Database Layer
   ↓
Core Banking Database
```

---

## 🗄️ Database Schema Overview

### 1️⃣ Customer Layer

• **accounts**
 Stores customer account details, status, and balances

• **cards**
 Debit/Credit cards mapped to accounts with priority & status

• **currency**
 Supported currency master table

---

### 2️⃣ Tokenization & Security

• **ringpay_tokens**
 Secure wearable payment tokens with usage limits & expiry controls

---

### 3️⃣ Channel Transaction Tables

Each payment channel has an isolated transaction table:

• **transaction_atm**
 ATM withdrawals, balance inquiry, mini statements

• **transaction_pos**
 POS purchases and refunds

• **transaction_ecom**
 E-commerce payments and reversals

• **transaction_mobile**
 Mobile banking transfers and payments

• **transaction_qrcode**
 QR-based merchant payments

• **transaction_ringpay**
 Wearable contactless payments

---

### 4️⃣ Risk & Fraud Monitoring

• **transaction_falcon**
 Fraud detection logs, suspicious activity records, declined transactions

---

### 5️⃣ Central Transaction Registry

• **transactions**
 Master table linking all channel transactions for audit & reconciliation

---

## ⚙️ Key Features

### 💳 Multi-Channel Transaction Processing

Supports:

* ATM Banking
* POS Payments
* Mobile Banking
* QR Payments
* E-commerce Transactions
* Wearable (RingPay) Payments

---

### 🛡️ Fraud Detection Engine (Falcon)

* Real-time fraud checks
* Risk scoring
* Fraud logging
* Automatic transaction blocking

---

### 🔐 Secure Payment Design

* Card priority handling
* Tokenized wearable payments
* Transaction limits (hourly/daily)
* Balance validation
* Masked PAN handling

---

### 🔄 Centralized Database Connectivity

A shared database module ensures:

✔ Single reusable DB session
✔ Faster processing
✔ Cleaner architecture
✔ Easy credential management

---

## 🛠️ Technology Stack

| Layer        | Technology             |
| ------------ | ---------------------- |
| Backend      | C++                    |
| API Handling | Custom Parser & Router |
| Database     | MySQL (MySQL X DevAPI) |
| Data Format  | JSON (nlohmann/json)   |
| Fraud Engine | Custom Falcon Engine   |
| Build System | CMake                  |

---

## 📂 Project Structure

```
CardAPIServer
 ┣ include/
 ┣ Database.cpp / Database.h
 ┣ atm.cpp
 ┣ pos.cpp
 ┣ ecom.cpp
 ┣ mobile.cpp
 ┣ qrcode.cpp
 ┣ ringpay.cpp
 ┣ issue.cpp
 ┣ falcon.cpp
 ┣ parser&router.cpp
 ┗ Create DB.sql
```

---

## 🚀 Transaction Flow (Example: Mobile Transfer)

1. Client sends transaction request
2. Request validated
3. Fraud engine risk analysis
4. Account & balance verification
5. Limit checks (hourly/daily)
6. Funds debited/credited
7. Channel transaction recorded
8. Master registry updated
9. Response returned to client

---

## 🔒 Security Controls

• Transaction limits
• Fraud pattern detection
• Token expiry enforcement
• Secure card mapping
• Exception handling & rollback
• Channel-wise transaction isolation

---

## 📈 Scalability Design

The architecture supports:

* Adding new payment channels easily
* High-volume transaction processing
* Independent channel upgrades
* Central monitoring & auditing

---

## 🧪 Build & Run

```bash
mkdir build
cd build
cmake ..
make
./CardAPIServer
```

---

## 📞 Contact

**Developer:** Rohan Sakhare
📱 +91 9112765649

---

## ⚠️ Disclaimer

This project is developed for educational and system-design demonstration purposes.
Unauthorized copying or production deployment without proper security review is discouraged.

---

⭐ If you found this project useful, consider giving it a star!
