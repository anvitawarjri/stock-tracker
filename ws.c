#include "ws.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#define WS_MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static void base64_encode(const unsigned char *input, int length, char *output) {
    EVP_EncodeBlock((unsigned char *)output, input, length);
}

int ws_handshake(int fd, void *ssl, char *token_out, size_t token_max) {
    (void)fd;
    SSL *ssl_conn = (SSL *)ssl;
    char buffer[4096];
    int bytes = SSL_read(ssl_conn, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) return 0;
    buffer[bytes] = '\0';

    if (strstr(buffer, "GET ") == NULL || strstr(buffer, "Upgrade: websocket") == NULL) {
        return 0; // Not a WebSocket handshake
    }

    char *key_start = strstr(buffer, "Sec-WebSocket-Key: ");
    if (!key_start) return 0;
    key_start += 19;
    char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return 0;

    char ws_key[64] = {0};
    int key_len = key_end - key_start;
    if (key_len >= (int)sizeof(ws_key)) return 0;
    strncpy(ws_key, key_start, key_len);

    if (token_out && token_max > 0) {
        token_out[0] = '\0';
        char *path_start = strstr(buffer, "GET ");
        if (path_start) {
            path_start += 4;
            char *token_param = strstr(path_start, "?token=");
            char *space = strchr(path_start, ' ');
            if (token_param && space && token_param < space) {
                token_param += 7;
                int tlen = space - token_param;
                if (tlen > 0 && tlen < (int)token_max) {
                    strncpy(token_out, token_param, tlen);
                    token_out[tlen] = '\0';
                }
            }
        }
        if (strlen(token_out) == 0) {
            char *auth = strstr(buffer, "Authorization: Bearer ");
            if (auth) {
                auth += 22;
                char *auth_end = strstr(auth, "\r\n");
                if (auth_end) {
                    int tlen = auth_end - auth;
                    if (tlen < (int)token_max) {
                        strncpy(token_out, auth, tlen);
                        token_out[tlen] = '\0';
                    }
                }
            }
        }
    }

    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", ws_key, WS_MAGIC_STRING);
    
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)combined, strlen(combined), sha1_hash);
    
    char base64_hash[64];
    base64_encode(sha1_hash, SHA_DIGEST_LENGTH, base64_hash);

    char response[512];
    snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        base64_hash);

    SSL_write(ssl_conn, response, strlen(response));
    return 1;
}

int ws_send_text(void *ssl, const char *text) {
    SSL *ssl_conn = (SSL *)ssl;
    size_t len = strlen(text);
    
    unsigned char frame[10];
    int header_len = 0;
    
    frame[0] = 0x81; 
    
    if (len <= 125) {
        frame[1] = (unsigned char)len;
        header_len = 2;
    } else if (len <= 65535) {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        header_len = 4;
    } else {
        frame[1] = 127;
        for (int i=0; i<8; i++) {
            frame[2+i] = (len >> (8*(7-i))) & 0xFF;
        }
        header_len = 10;
    }

    int ret1 = SSL_write(ssl_conn, frame, header_len);
    if (ret1 <= 0) return 0;
    int ret2 = SSL_write(ssl_conn, text, len);
    if (ret2 <= 0) return 0;
    
    return 1;
}
