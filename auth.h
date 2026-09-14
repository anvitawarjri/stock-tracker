#ifndef AUTH_H
#define AUTH_H

// Validate an authentication token against the hardcoded list
// Returns 1 if valid, 0 if invalid
int auth_validate_token(const char *token);

#endif
