#ifndef WS_H
#define WS_H

#include <stddef.h>

// Handle the WebSocket handshake
// Returns 1 on success, 0 on failure
// Extracted token will be populated in 'token_out' if provided
int ws_handshake(int fd, void *ssl, char *token_out, size_t token_max);

// Send a text frame over WebSocket
int ws_send_text(void *ssl, const char *text);

#endif
