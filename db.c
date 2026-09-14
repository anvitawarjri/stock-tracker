#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sqlite3* db_init(const char *filename) {
    sqlite3 *db;
    if (sqlite3_open_v2(filename, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) {
            fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
            sqlite3_close(db);
        } else {
            fprintf(stderr, "Cannot open database\n");
        }
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    
    const char *sql = "CREATE TABLE IF NOT EXISTS stock_prices ("
                      "id INTEGER PRIMARY KEY, "
                      "symbol TEXT, "
                      "price REAL, "
                      "change_val REAL, "
                      "ts INTEGER)";
    char *err_msg = NULL;
    if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL error creating stock_prices: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", 0, 0, NULL);

    return db;
}

void db_close(sqlite3 *db) {
    if (db) {
        sqlite3_close(db);
    }
}

int db_insert_price(sqlite3 *db, const char *symbol, double price, double change_val, long long ts) {
    if (!db) return 0;
    const char *sql = "INSERT INTO stock_prices (symbol, price, change_val, ts) VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    
    sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, price);
    sqlite3_bind_double(stmt, 3, change_val);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)ts);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

char* db_get_history_json(sqlite3 *db, const char *symbol) {
    if (!db) return strdup("[]");
    
    const char *sql = "SELECT price, change_val, ts FROM (SELECT price, change_val, ts FROM stock_prices WHERE symbol = ? ORDER BY ts DESC LIMIT 100) ORDER BY ts ASC";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return strdup("[]");
    
    sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_TRANSIENT);
    
    size_t buf_size = 8192; 
    char *json = malloc(buf_size);
    if (!json) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    
    strcpy(json, "[");
    int first = 1;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double price = sqlite3_column_double(stmt, 0);
        double change = sqlite3_column_double(stmt, 1);
        long long ts = sqlite3_column_int64(stmt, 2);
        
        char record[128];
        snprintf(record, sizeof(record), "%s{\"price\":%.2f,\"change\":%.2f,\"ts\":%lld}", 
                 first ? "" : ",", price, change, ts);
        
        if (strlen(json) + strlen(record) < buf_size - 2) {
            strcat(json, record);
        }
        first = 0;
    }
    strcat(json, "]");
    
    sqlite3_finalize(stmt);
    return json;
}

char* db_get_logs_json(sqlite3 *db) {
    if (!db) return strdup("[]");
    
    const char *sql = "SELECT ts, level, message FROM syslogs ORDER BY ts DESC LIMIT 50";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return strdup("[]");
    
    size_t buf_size = 16384; 
    char *json = malloc(buf_size);
    if (!json) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    
    strcpy(json, "[");
    int first = 1;
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        long long ts = sqlite3_column_int64(stmt, 0);
        const unsigned char *level = sqlite3_column_text(stmt, 1);
        const unsigned char *message = sqlite3_column_text(stmt, 2);
        
        char msg_esc[1024] = {0};
        int j = 0;
        for (int i=0; message && message[i] != '\0' && j < 1000; i++) {
            if (message[i] == '"') {
                msg_esc[j++] = '\\'; msg_esc[j++] = '"';
            } else if (message[i] == '\n') {
                msg_esc[j++] = '\\'; msg_esc[j++] = 'n';
            } else if (message[i] == '\r') {
                // skip
            } else if (message[i] == '\\') {
                msg_esc[j++] = '\\'; msg_esc[j++] = '\\';
            } else {
                msg_esc[j++] = message[i];
            }
        }
        
        char record[2048];
        snprintf(record, sizeof(record), "%s{\"ts\":%lld,\"level\":\"%s\",\"message\":\"%s\"}", 
                 first ? "" : ",", ts, level ? (const char*)level : "", msg_esc);
        
        if (strlen(json) + strlen(record) < buf_size - 2) {
            strcat(json, record);
        }
        first = 0;
    }
    strcat(json, "]");
    
    sqlite3_finalize(stmt);
    return json;
}
