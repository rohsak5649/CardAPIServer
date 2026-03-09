#ifndef DATABASE_H
#define DATABASE_H

#include <mysqlx/xdevapi.h>
using namespace mysqlx;

class Database {
public:
    static Session& getSession();
    static Schema getSchema();

private:
    static Session* session;
};

#endif