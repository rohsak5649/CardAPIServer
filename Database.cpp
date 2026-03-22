/*
* Copyright (c) Rohan Sakhare
* All rights reserved.
*
* DATABASE CONNECTION & SESSION MANAGEMENT FLOW:
*
* 1. PURPOSE:
*    - Provides a centralized database access layer.
*    - Manages MySQL X DevAPI session lifecycle.
*    - Ensures single shared session across application.
*
* 2. SINGLETON SESSION MANAGEMENT:
*    - Static Session pointer used to maintain single instance.
*    - Lazy initialization:
*        → Session created only when first requested.
*    - Prevents multiple DB connections and reduces overhead.
*
* 3. CONNECTION INITIALIZATION:
*    - Establishes connection to MySQL server:
*        → Host: localhost
*        → Port: 33060 (X Protocol)
*        → User: root
*    - Executes "USE bankingdb" to set default schema.
*
* 4. getSession():
*    - Returns active database session reference.
*    - If session not initialized:
*        → Creates new session
*        → Sets active schema
*    - Ensures all modules share same DB connection.
*
* 5. getSchema():
*    - Returns Schema object for "bankingdb".
*    - Used by repositories/services for table access.
*
* 6. USAGE ACROSS MODULES:
*    - Used by:
*        → ECOM transactions
*        → MOBILE transactions
*        → ATM transactions
*        → FALCON fraud engine
*
* 7. PERFORMANCE BENEFITS:
*    - Avoids repeated DB connection creation.
*    - Improves latency for high-frequency transactions.
*
* 8. SECURITY NOTES:
*    - Credentials should NOT be hardcoded in production.
*    - Recommended:
*        → Environment variables
*        → Secure vault (AWS Secrets Manager / HashiCorp Vault)
*
* 9. LIMITATIONS:
*    - Current implementation is NOT thread-safe.
*    - In high concurrency systems:
*        → Use connection pooling
*        → Use thread-local sessions
*
* 10. FUTURE ENHANCEMENTS:
*    - Add connection pooling support.
*    - Add automatic reconnect handling.
*    - Add logging for DB connection lifecycle.
*
* Unauthorized modification without understanding
* session lifecycle and concurrency implications is discouraged.
*
* For implementation details, contact: +91 9112765649
*/

#include "Database.h"

Session* Database::session = nullptr;

Session& Database::getSession() {
    if (!session) {
        session = new Session("localhost", 33060, "root", "Rohan@5649");
        session->sql("USE bankingdb").execute();
    }
    return *session;
}

Schema Database::getSchema() {
    return getSession().getSchema("bankingdb");
}