#include "auth.h"
#include <string.h>

static const char *VALID_TOKENS[] = {
    "token123",
    "supersecret",
    "group14token",
    NULL
};

int auth_validate_token(const char *token) {
    if (!token) return 0;
    for (int i = 0; VALID_TOKENS[i] != NULL; i++) {
        if (strcmp(token, VALID_TOKENS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}
