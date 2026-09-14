#ifndef DB_H
#define DB_H

#include <sqlite3.h>

// Initialize database
sqlite3* db_init(const char *filename);

// Close database
void db_close(sqlite3 *db);

// Insert a price tick
int db_insert_price(sqlite3 *db, const char *symbol, double price, double change_val, long long ts);

// Get history for a symbol as a JSON array string. Caller must free the string.
char* db_get_history_json(sqlite3 *db, const char *symbol);

// Get recent logs as a JSON array string. Caller must free the string.
char* db_get_logs_json(sqlite3 *db);

#endif
