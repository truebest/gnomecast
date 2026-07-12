#ifndef GNOMECAST_UI_HOST_H
#define GNOMECAST_UI_HOST_H

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline const char *native_ui_host_accepted_chars(void) {
    return ".-_:[]0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
}

/* The ADDRESS and PORT fields are separate, so the worker needs a bare host.
 * Accept the familiar bracketed spelling for IPv6 literals, but strip its
 * delimiters and reject any malformed use of brackets. */
static inline bool native_ui_host_normalize(const char *host, char *normalized,
                                            size_t normalized_cap) {
    if (!host || !host[0] || !normalized || normalized_cap == 0u ||
        host[strspn(host, native_ui_host_accepted_chars())] != '\0') {
        return false;
    }

    size_t len = strlen(host);
    bool has_open_bracket = strchr(host, '[') != NULL;
    bool has_close_bracket = strchr(host, ']') != NULL;
    if (has_open_bracket || has_close_bracket) {
        if (len < 3u || host[0] != '[' || host[len - 1u] != ']') {
            return false;
        }

        size_t literal_len = len - 2u;
        if (literal_len >= INET6_ADDRSTRLEN ||
            memchr(host + 1, '[', literal_len) != NULL ||
            memchr(host + 1, ']', literal_len) != NULL) {
            return false;
        }

        char literal[INET6_ADDRSTRLEN];
        memcpy(literal, host + 1, literal_len);
        literal[literal_len] = '\0';
        struct in6_addr address;
        if (inet_pton(AF_INET6, literal, &address) != 1 || literal_len >= normalized_cap) {
            return false;
        }
        memmove(normalized, host + 1, literal_len);
        normalized[literal_len] = '\0';
        return true;
    }

    if (len >= normalized_cap) {
        return false;
    }
    memmove(normalized, host, len + 1u);
    return true;
}

#endif
