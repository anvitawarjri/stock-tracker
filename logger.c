#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static sqlite3 *g_db = NULL;

int logger_init(sqlite3 *db) {
    g_db = db;
    const char *sql = "CREATE TABLE IF NOT EXISTS syslogs ("
                      "id INTEGER PRIMARY KEY, "
                      "ts INTEGER, "
                      "level TEXT, "
                      "message TEXT)";
    char *err_msg = NULL;
    if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK) {
        fprintf(stderr, "SQL error in logger_init: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }
    return 1;
}

void log_event(const char *level, const char *fmt, ...) {
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    time_t now = time(NULL);

    // Make ctime string without trailing newline
    char time_str[64];
    ctime_r(&now, time_str);
    size_t ln = strlen(time_str);
    if (ln > 0 && time_str[ln-1] == '\n') time_str[ln-1] = '\0';

    // Write to stderr
    fprintf(stderr, "[%s] %s: %s\n", level, time_str, message);

    // Insert into DB
    if (g_db) {
        const char *sql = "INSERT INTO syslogs (ts, level, message) VALUES (?, ?, ?)";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);
            sqlite3_bind_text(stmt, 2, level, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, message, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}
