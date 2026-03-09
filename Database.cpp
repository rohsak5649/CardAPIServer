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