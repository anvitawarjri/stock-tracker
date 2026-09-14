#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <time.h>

int connect_to_server(const char *ip, int port, SSL_CTX *ctx, SSL **ssl_out, const char *token) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        close(fd);
        return -1;
    }
    
    char request[512];
    snprintf(request, sizeof(request),
             "GET /?token=%s HTTP/1.1\r\n"
             "Host: localhost:8443\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n", token);
             
    SSL_write(ssl, request, strlen(request));
    
    char buffer[1024];
    int bytes = SSL_read(ssl, buffer, sizeof(buffer)-1);
    if (bytes <= 0) {
        SSL_free(ssl);
        close(fd);
        return -1;
    }
    buffer[bytes] = '\0';
    
    if (strstr(buffer, "101 Switching Protocols") == NULL) {
        SSL_free(ssl);
        close(fd);
        return -2;
    }
    
    *ssl_out = ssl;
    return fd;
}

struct ClientArg {
    int id;
    int duration;
    const char *token;
    int is_malicious;
    int abrupt_close;
    int messages_received;
};

volatile int g_running = 1;

void* client_thread(void *arg) {
    struct ClientArg *carg = (struct ClientArg*)arg;
    
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); 
    
    SSL *ssl = NULL;
    int fd = connect_to_server("127.0.0.1", 8443, ctx, &ssl, carg->token);
    
    if (carg->is_malicious) {
        if (fd == -2) {
            carg->messages_received = -1; 
        }
        if (fd >= 0) {
            SSL_free(ssl);
            close(fd);
        }
        SSL_CTX_free(ctx);
        return NULL;
    }
    
    if (fd < 0) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    
    time_t start = time(NULL);
    char buf[1024];
    
    while (g_running && (time(NULL) - start) < carg->duration) {
        if (carg->abrupt_close && (time(NULL) - start) > 5) {
            struct linger sl;
            sl.l_onoff = 1;
            sl.l_linger = 0;
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
            close(fd); 
            SSL_CTX_free(ctx);
            return NULL;
        }
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval tv = {1, 0};
        int ret = select(fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0) {
            int bytes = SSL_read(ssl, buf, sizeof(buf));
            if (bytes > 0) {
                carg->messages_received++;
            } else {
                printf("Client disconnected! SSL_read error: %d\n", SSL_get_error(ssl, bytes)); ERR_print_errors_fp(stdout); break;
            }
        }
    }
    
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    SSL_CTX_free(ctx);
    
    return NULL;
}

int main(int argc, char **argv) {
    int num_clients = 100;
    if (argc > 1) {
        num_clients = atoi(argv[1]);
    }
    
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    
    int total_threads = num_clients + 5 + 3; 
    pthread_t *threads = malloc(total_threads * sizeof(pthread_t));
    struct ClientArg *args = malloc(total_threads * sizeof(struct ClientArg));
    
    printf("Starting stress test with %d normal clients, 5 malicious, 3 abrupt close.\n", num_clients);
    
    for (int i=0; i<total_threads; i++) {
        args[i].id = i;
        args[i].duration = 30;
        args[i].messages_received = 0;
        
        if (i < num_clients) {
            args[i].token = "token123";
            args[i].is_malicious = 0;
            args[i].abrupt_close = 0;
        } else if (i < num_clients + 5) {
            args[i].token = "invalidtoken";
            args[i].is_malicious = 1;
            args[i].abrupt_close = 0;
        } else {
            args[i].token = "token123";
            args[i].is_malicious = 0;
            args[i].abrupt_close = 1;
        }
        
        pthread_create(&threads[i], NULL, client_thread, &args[i]);
    }
    
    for (int i=0; i<30; i++) {
        sleep(1);
        printf("\rRunning... %d/30s", i+1);
        fflush(stdout);
    }
    printf("\nFinishing up...\n");
    g_running = 0;
    
    int total_msgs = 0;
    int malicious_rejected = 0;
    for (int i=0; i<total_threads; i++) {
        pthread_join(threads[i], NULL);
        if (i < num_clients) {
            total_msgs += args[i].messages_received;
        } else if (i >= num_clients && i < num_clients + 5) {
            if (args[i].messages_received == -1) malicious_rejected++;
        }
    }
    
    printf("=== Stress Test Report ===\n");
    printf("Total Messages Received (Normal Clients): %d\n", total_msgs);
    printf("Average Msgs/Client: %.2f\n", (double)total_msgs / num_clients);
    printf("Msgs/Sec/Client (approx): %.2f\n", (double)total_msgs / num_clients / 30.0);
    printf("Malicious Clients Rejected: %d/5\n", malicious_rejected);
    
    free(threads);
    free(args);
    return 0;
}
