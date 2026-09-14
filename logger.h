#ifndef LOGGER_H
#define LOGGER_H

#include <sqlite3.h>

// Initialize the logger (creates SQLite syslogs table)
int logger_init(sqlite3 *db);

// Log an event
void log_event(const char *level, const char *fmt, ...);

#endif
