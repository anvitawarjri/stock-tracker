#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "ws.h"
#include "db.h"
#include "logger.h"
#include "auth.h"
#include "stock.h"

#define WS_PORT 8443
#define HTTP_PORT 8081
#define MAX_CLIENTS 1024
#define DB_FILE "stocktracker.db"

volatile int g_shutdown_flag = 0;
sqlite3 *g_server_db = NULL;

typedef struct {
    SSL *ssl;
    int fd;
    int active;
    char token[64];
    time_t connected_at;
    pthread_mutex_t ssl_mutex;
} Client;

Client clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

SSL_CTX *create_context() {
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

void configure_context(SSL_CTX *ctx) {
    if (SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

void handle_signal(int sig) {
    (void)sig;
    g_shutdown_flag = 1;
}

int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    int sndbuf = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    int rcvbuf = 64 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    if (listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void broadcast_update(const char *json_msg) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].ssl) {
            pthread_mutex_lock(&clients[i].ssl_mutex);
            ws_send_text(clients[i].ssl, json_msg);
            pthread_mutex_unlock(&clients[i].ssl_mutex);
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    clock_gettime(CLOCK_MONOTONIC, &end);
    long long diff_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
    double latency_ms = diff_ns / 1000000.0;
    
    if (latency_ms > 10.0) {
        log_event("PERF", "Broadcast latency > 10ms: %.2f ms", latency_ms);
    }
}

typedef struct {
    int fd;
    SSL_CTX *ctx;
} ClientParams;

void* client_thread_func(void* arg) {
    ClientParams *params = (ClientParams*)arg;
    int client_fd = params->fd;
    SSL_CTX *ctx = params->ctx;
    free(params);

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) <= 0) {
        log_event("ERROR", "TLS accept failed");
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    char token[64] = {0};
    if (!ws_handshake(client_fd, ssl, token, sizeof(token))) {
        log_event("ERROR", "WebSocket handshake failed");
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    if (!auth_validate_token(token)) {
        log_event("AUTH", "Auth failed for token: %s", token);
        const char *http_401 = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, http_401, strlen(http_401));
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    log_event("AUTH", "Client connected (token: %s)", token);

    int client_index = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].active = 1;
            clients[i].fd = client_fd;
            clients[i].ssl = ssl;
            strncpy(clients[i].token, token, sizeof(clients[i].token)-1);
            clients[i].connected_at = time(NULL);
            client_index = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (client_index == -1) {
        log_event("ERROR", "Max clients reached, dropping connection");
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    char buf[1024];
    while (!g_shutdown_flag) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        struct timeval tv = {1, 0};
        int ret = select(client_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            pthread_mutex_lock(&clients[client_index].ssl_mutex);
            int bytes = SSL_read(ssl, buf, sizeof(buf));
            pthread_mutex_unlock(&clients[client_index].ssl_mutex);
            if (bytes <= 0) {
                int err = SSL_get_error(ssl, bytes);
                if (err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL || err == SSL_ERROR_SSL) {
                    break;
                }
            }
        }
    }

    log_event("INFO", "Client disconnected (token: %s)", token);

    pthread_mutex_lock(&clients_mutex);
    if (client_index >= 0 && client_index < MAX_CLIENTS) {
        clients[client_index].active = 0;
        clients[client_index].ssl = NULL;
        clients[client_index].fd = -1;
    }
    pthread_mutex_unlock(&clients_mutex);

    SSL_free(ssl);
    close(client_fd);
    return NULL;
}

void* http_server_thread(void* arg) {
    (void)arg;
    int fd = create_server_socket(HTTP_PORT);
    if (fd < 0) {
        log_event("ERROR", "Failed to start HTTP server on %d", HTTP_PORT);
        return NULL;
    }
    log_event("INFO", "HTTP server listening on %d", HTTP_PORT);

    while (!g_shutdown_flag) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval tv = {1, 0};
        int ret = select(fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(fd, (struct sockaddr*)&client_addr, &len);
            if (client_fd < 0) continue;

            char buffer[2048];
            int bytes = read(client_fd, buffer, sizeof(buffer)-1);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                char method[16], path[256];
                if (sscanf(buffer, "%15s %255s", method, path) == 2) {
                    if (strcmp(method, "OPTIONS") == 0) {
                        const char *resp = "HTTP/1.1 200 OK\r\n"
                                           "Access-Control-Allow-Origin: *\r\n"
                                           "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                                           "Access-Control-Allow-Headers: Authorization\r\n"
                                           "Connection: close\r\n\r\n";
                        write(client_fd, resp, strlen(resp));
                        close(client_fd);
                        continue;
                    } else if (strcmp(method, "GET") == 0) {
                        int is_api = (strncmp(path, "/api/", 5) == 0);
                        int authorized = 0;
                        if (is_api) {
                            char *auth_header = strstr(buffer, "Authorization: Bearer ");
                            if (!auth_header) auth_header = strstr(buffer, "authorization: Bearer ");
                            
                            if (auth_header) {
                                auth_header += 22;
                                char token[64] = {0};
                                char *end = strstr(auth_header, "\r\n");
                                if (end && (size_t)(end - auth_header) < sizeof(token)) {
                                    strncpy(token, auth_header, end - auth_header);
                                    authorized = auth_validate_token(token);
                                }
                            }
                        }

                        if (is_api && !authorized) {
                            const char *resp = "HTTP/1.1 401 Unauthorized\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
                            write(client_fd, resp, strlen(resp));
                        } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
                            FILE *f = fopen("index.html", "r");
                            if (f) {
                                fseek(f, 0, SEEK_END);
                                long fsize = ftell(f);
                                fseek(f, 0, SEEK_SET);
                                char *html = malloc(fsize + 1);
                                fread(html, 1, fsize, f);
                                fclose(f);
                                html[fsize] = '\0';

                                char header[256];
                                snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", fsize);
                                write(client_fd, header, strlen(header));
                                write(client_fd, html, fsize);
                                free(html);
                            } else {
                                const char *resp = "HTTP/1.1 404 Not Found\r\n\r\n";
                                write(client_fd, resp, strlen(resp));
                            }
                        } else if (strncmp(path, "/api/history?symbol=", 20) == 0) {
                            char *symbol = path + 20;
                            char *json = db_get_history_json(g_server_db, symbol);
                            if (json) {
                                char header[256];
                                snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", strlen(json));
                                write(client_fd, header, strlen(header));
                                write(client_fd, json, strlen(json));
                                free(json);
                            } else {
                                const char *resp = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
                                write(client_fd, resp, strlen(resp));
                            }
                        } else if (strcmp(path, "/api/logs") == 0) {
                            char *json = db_get_logs_json(g_server_db);
                            if (json) {
                                char header[256];
                                snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n", strlen(json));
                                write(client_fd, header, strlen(header));
                                write(client_fd, json, strlen(json));
                                free(json);
                            } else {
                                const char *resp = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
                                write(client_fd, resp, strlen(resp));
                            }
                        } else {
                            const char *resp = "HTTP/1.1 404 Not Found\r\n\r\n";
                            write(client_fd, resp, strlen(resp));
                        }
                    }
                }
            }
            close(client_fd);
        }
    }
    close(fd);
    return NULL;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN); 

    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_init(&clients[i].ssl_mutex, NULL);
    }

    g_server_db = db_init(DB_FILE);
    if (!g_server_db) return EXIT_FAILURE;

    logger_init(g_server_db);
    log_event("INFO", "Server starting up...");

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    
    SSL_CTX *ctx = create_context();
    configure_context(ctx);

    stock_simulator_start();

    pthread_t http_thread;
    pthread_create(&http_thread, NULL, http_server_thread, NULL);

    int server_fd = create_server_socket(WS_PORT);
    if (server_fd < 0) {
        log_event("ERROR", "Failed to start TLS server on %d", WS_PORT);
        return EXIT_FAILURE;
    }
    
    log_event("INFO", "TLS WebSocket server listening on %d", WS_PORT);

    int sbuf = 0, rbuf = 0;
    socklen_t optlen = sizeof(int);
    getsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &sbuf, &optlen);
    getsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &rbuf, &optlen);
    log_event("INFO", "Socket config: SO_SNDBUF=%d, SO_RCVBUF=%d", sbuf, rbuf);

    while (!g_shutdown_flag) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            int client_fd = accept(server_fd, (struct sockaddr*)&addr, &len);
            if (client_fd >= 0) {
                ClientParams *params = malloc(sizeof(ClientParams));
                params->fd = client_fd;
                params->ctx = ctx;
                
                pthread_t tid;
                pthread_create(&tid, NULL, client_thread_func, params);
                pthread_detach(tid);
            }
        }
    }

    log_event("INFO", "Shutting down gracefully...");
    
    stock_simulator_stop();
    pthread_join(http_thread, NULL);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].ssl) {
            SSL_shutdown(clients[i].ssl);
            SSL_free(clients[i].ssl);
            close(clients[i].fd);
            clients[i].active = 0;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    close(server_fd);
    SSL_CTX_free(ctx);
    db_close(g_server_db);
    
    EVP_cleanup();
    log_event("INFO", "Shutdown complete.");
    return 0;
}
