#include "stock.h"
#include "db.h"
#include "logger.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void broadcast_update(const char *json_msg);
extern sqlite3 *g_server_db;
extern volatile int g_shutdown_flag;

typedef struct {
  const char *symbol;
  double price;
} Stock;

static Stock stocks[] = {{"AAPL", 170.0}, {"GOOG", 140.0}, {"TSLA", 180.0},
                         {"MSFT", 400.0}, {"AMZN", 175.0}, {"NVDA", 850.0},
                         {"META", 500.0}, {"NFLX", 600.0}, {"AMD", 160.0},
                         {"INTC", 40.0}};
static const int num_stocks = 10;
static pthread_t stock_thread;

static void *stock_worker(void *arg) {
  (void)arg;
  const char *api_key = "d7m6jkpr01qk7lvv8040d7m6jkpr01qk7lvv804g";
  if (!api_key) {
    log_event("WARN", "FINNHUB_API_KEY not set. Using dummy data.");
  } else {
    log_event("INFO", "FINNHUB_API_KEY found. Fetching real data.");
  }

  int current_stock_idx = 0;

  while (!g_shutdown_flag) {
    if (api_key) {
      usleep(1500000); // 1.5s interval -> 40 reqs/min
    } else {
      usleep(500000);
    }

    if (g_shutdown_flag)
      break;

    if (g_server_db) {
      sqlite3_exec(g_server_db, "BEGIN TRANSACTION;", 0, 0, NULL);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long current_time = (long long)ts.tv_sec;

    if (api_key) {
      int i = current_stock_idx;
      char cmd[512];
      snprintf(cmd, sizeof(cmd),
               "curl -s 'https://finnhub.io/api/v1/quote?symbol=%s&token=%s'",
               stocks[i].symbol, api_key);

      FILE *fp = popen(cmd, "r");
      if (fp) {
        char buffer[1024] = {0};
        size_t read_bytes = fread(buffer, 1, sizeof(buffer) - 1, fp);
        pclose(fp);

        if (read_bytes > 0) {
          char *c_ptr = strstr(buffer, "\"c\":");
          char *d_ptr = strstr(buffer, "\"d\":");

          if (c_ptr && d_ptr) {
            double price = 0, change = 0;
            sscanf(c_ptr, "\"c\":%lf", &price);
            sscanf(d_ptr, "\"d\":%lf", &change);

            if (price > 0) {
              stocks[i].price = price;

              char json_msg[256];
              snprintf(json_msg, sizeof(json_msg),
                       "{\"type\":\"price\",\"symbol\":\"%s\",\"price\":%.2f,"
                       "\"change\":%.2f,\"timestamp\":%lld}",
                       stocks[i].symbol, stocks[i].price, change, current_time);

              broadcast_update(json_msg);

              if (g_server_db) {
                db_insert_price(g_server_db, stocks[i].symbol, stocks[i].price,
                                change, current_time);
              }
            }
          }
        }
      }
      current_stock_idx = (current_stock_idx + 1) % num_stocks;
    } else {
      for (int i = 0; i < num_stocks; i++) {
        double volatility = 1.0;
        double change_pct = ((rand() % 201) - 100) / 100.0 * volatility;
        double change_val = stocks[i].price * (change_pct / 100.0);
        stocks[i].price += change_val;

        char json_msg[256];
        snprintf(json_msg, sizeof(json_msg),
                 "{\"type\":\"price\",\"symbol\":\"%s\",\"price\":%.2f,"
                 "\"change\":%.2f,\"timestamp\":%lld}",
                 stocks[i].symbol, stocks[i].price, change_val, current_time);

        broadcast_update(json_msg);

        if (g_server_db) {
          db_insert_price(g_server_db, stocks[i].symbol, stocks[i].price,
                          change_val, current_time);
        }
      }
    }

    if (g_server_db) {
      sqlite3_exec(g_server_db, "COMMIT;", 0, 0, NULL);
    }
  }
  return NULL;
}

void stock_simulator_start(void) {
  pthread_create(&stock_thread, NULL, stock_worker, NULL);
}

void stock_simulator_stop(void) { pthread_join(stock_thread, NULL); }
