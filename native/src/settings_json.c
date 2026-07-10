#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "settings_json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config_paths.h"

const char *native_session_slot_name(int slot) {
    switch (slot) {
    case NATIVE_SESSION_SLOT_RED:
        return "red";
    case NATIVE_SESSION_SLOT_GREEN:
        return "green";
    case NATIVE_SESSION_SLOT_YELLOW:
        return "yellow";
    case NATIVE_SESSION_SLOT_BLUE:
        return "blue";
    default:
        return "?";
    }
}

void native_settings_defaults(NativeSettings *settings) {
    memset(settings, 0, sizeof(*settings));
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        settings->sessions[i].port = 3389;
        settings->sessions[i].fps = 60;
    }
    (void)snprintf(settings->sessions[NATIVE_SESSION_SLOT_GREEN].host,
                   sizeof(settings->sessions[NATIVE_SESSION_SLOT_GREEN].host), "127.0.0.1");
    settings->width = 1920;
    settings->height = 1080;
    settings->wheel_step = 60;
    settings->wheel_scroll_divisor = 1;
    settings->audio_codec = NATIVE_AUDIO_CODEC_AUTO;
}

void native_settings_warn_deprecated_audio_prebuffer(void) {
    static bool warned;
    if (!warned) {
        fprintf(stderr,
                "[native-audio] audioPrebufferMs/--audio-prebuffer-ms is deprecated and ignored; adaptive buffering is always enabled\n");
        warned = true;
    }
}

const char *native_json_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

const char *native_json_find_value(const char *json, const char *key) {
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return NULL;
    }

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after_key = native_json_skip_ws(p + (size_t)n);
        if (*after_key == ':') {
            return native_json_skip_ws(after_key + 1);
        }
        p += (size_t)n;
    }
    return NULL;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int native_json_read_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }
    if (*p != '"') {
        return -1;
    }
    p++;

    size_t written = 0;
    while (*p && *p != '"') {
        unsigned char ch = (unsigned char)*p++;
        if (ch < 0x20) {
            return -1;
        }
        if (ch == '\\') {
            ch = (unsigned char)*p++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u': {
                /* Read the four hex digits left to right, stopping at the first non-hex
                 * character. hex_value('\0') is negative, so a string ending inside the
                 * escape (e.g. "\u" or "\u0") short-circuits before p[1..3] are touched —
                 * argv-backed launch-parameter strings have no trailing slack bytes, so an
                 * unconditional p[1..3] read there is out of bounds. */
                int h0 = hex_value(p[0]);
                int h1 = h0 < 0 ? -1 : hex_value(p[1]);
                int h2 = h1 < 0 ? -1 : hex_value(p[2]);
                int h3 = h2 < 0 ? -1 : hex_value(p[3]);
                if (h0 != 0 || h1 != 0 || h2 < 0 || h3 < 0) {
                    return -1;
                }
                ch = (unsigned char)((h2 << 4) | h3);
                p += 4;
                break;
            }
            default:
                return -1;
            }
        }
        if (written + 1 >= cap) {
            return -1;
        }
        out[written++] = (char)ch;
    }

    if (*p != '"') {
        return -1;
    }
    out[written] = '\0';
    return 1;
}

int native_json_read_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(p, &end, 10);
    if (errno != 0 || end == p || value < (unsigned long)min_value || value > (unsigned long)max_value) {
        return -1;
    }
    *out = (uint16_t)value;
    return 1;
}

int native_json_read_bool(const char *json, const char *key, bool *out) {
    const char *p = native_json_find_value(json, key);
    if (!p) {
        return 0;
    }
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return 1;
    }
    return -1;
}

static bool apply_json_string(const char *json, const char *key, char *dest, size_t cap, const char *source) {
    int result = native_json_read_string(json, key, dest, cap);
    if (result < 0) {
        fprintf(stderr, "[native] invalid string value for config field %s in %s\n", key, source);
        return false;
    }
    return true;
}

static bool apply_json_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *dest,
                           const char *source) {
    uint16_t value = 0;
    int result = native_json_read_u16(json, key, min_value, max_value, &value);
    if (result < 0) {
        fprintf(stderr, "[native] invalid numeric value for config field %s in %s\n", key, source);
        return false;
    }
    if (result > 0) {
        *dest = value;
    }
    return true;
}

/* Session-object fields shared by the legacy flat format and the v2 array entries. */
static bool apply_session_json(NativeSessionConfig *session, const char *json, const char *source) {
    return apply_json_string(json, "host", session->host, sizeof(session->host), source) &&
           apply_json_string(json, "username", session->username, sizeof(session->username), source) &&
           apply_json_string(json, "password", session->password, sizeof(session->password), source) &&
           apply_json_string(json, "domain", session->domain, sizeof(session->domain), source) &&
           apply_json_u16(json, "port", 1, UINT16_MAX, &session->port, source) &&
           apply_json_u16(json, "fps", 1, 240, &session->fps, source);
}

static bool apply_audio_codec_json(NativeSettings *settings, const char *json, const char *source) {
    char codec[16];
    int result = native_json_read_string(json, "audioCodec", codec, sizeof(codec));
    if (result == 0) {
        return true; /* absent: keep the current value */
    }
    if (result > 0) {
        if (strcmp(codec, "auto") == 0 || strcmp(codec, "opus") == 0) {
            settings->audio_codec = NATIVE_AUDIO_CODEC_AUTO;
            return true;
        }
        if (strcmp(codec, "pcm") == 0) {
            settings->audio_codec = NATIVE_AUDIO_CODEC_PCM;
            return true;
        }
    }
    fprintf(stderr, "[native] invalid value for config field audioCodec in %s\n", source);
    return false;
}

static bool apply_global_json(NativeSettings *settings, const char *json, const char *source) {
    uint16_t ignored_prebuffer = 0;
    int deprecated = native_json_read_u16(json, "audioPrebufferMs", 0, 1000, &ignored_prebuffer);
    if (deprecated < 0) {
        fprintf(stderr, "[native] invalid value for deprecated config field audioPrebufferMs in %s\n", source);
        return false;
    }
    if (deprecated > 0) {
        native_settings_warn_deprecated_audio_prebuffer();
    }
    return apply_json_u16(json, "wheelStep", 1, 120, &settings->wheel_step, source) &&
           apply_json_u16(json, "wheelScrollDivisor", 1, 120, &settings->wheel_scroll_divisor, source) &&
           apply_audio_codec_json(settings, json, source);
}

/* Locates the JSON object for array entry `index` inside the "sessions" array. String-aware
 * brace matching (quotes and escapes are honored); no nested arrays are expected inside a
 * session object, but nested braces are handled anyway. Returns 1 when the entry was
 * found, 0 when the array ends cleanly before `index`, and -1 for a malformed document
 * (missing array, non-object entry, unterminated object/array) — the caller must NOT
 * treat -1 as an empty array, or a truncated settings file would parse as "success" and
 * mask an intact lower-priority candidate. */
static int find_session_object(const char *json, size_t index, const char **out_start, size_t *out_len) {
    const char *p = native_json_find_value(json, "sessions");
    if (!p || *p != '[') {
        return -1;
    }
    p = native_json_skip_ws(p + 1);

    size_t current = 0;
    for (;;) {
        if (*p == ']') {
            return 0; /* clean end of array */
        }
        if (*p != '{') {
            return -1; /* truncated document or non-object entry */
        }
        const char *start = p;
        int depth = 0;
        bool in_string = false;
        for (; *p; p++) {
            char c = *p;
            if (in_string) {
                if (c == '\\' && p[1]) {
                    p++;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (*p != '}') {
            return -1; /* unterminated object */
        }
        p++; /* past the closing brace */
        if (current == index) {
            *out_start = start;
            *out_len = (size_t)(p - start);
            return 1;
        }
        current++;
        p = native_json_skip_ws(p);
        if (*p == ',') {
            p = native_json_skip_ws(p + 1);
        } else if (*p != ']') {
            return -1; /* missing separator/terminator */
        }
    }
}

static int session_slot_from_object(const char *object_json, size_t fallback_index) {
    char slot_name[16];
    int result = native_json_read_string(object_json, "slot", slot_name, sizeof(slot_name));
    if (result > 0) {
        for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
            if (strcmp(slot_name, native_session_slot_name(slot)) == 0) {
                return slot;
            }
        }
        return -1; /* a slot tag we do not know — skip the entry */
    }
    if (result < 0) {
        /* Present but unreadable (over-long tag, non-string value): treat as unknown
         * rather than falling back to the array index, which would let a foreign tag
         * overwrite the green slot. */
        return -1;
    }
    return fallback_index < NATIVE_SETTINGS_MAX_SESSIONS ? (int)fallback_index : -1;
}

static bool apply_sessions_array(NativeSettings *settings, const char *json, const char *source) {
    for (size_t index = 0;; index++) {
        const char *start = NULL;
        size_t len = 0;
        int found = find_session_object(json, index, &start, &len);
        if (found == 0) {
            return true; /* clean end of array (possibly empty) */
        }
        if (found < 0) {
            fprintf(stderr, "[native] malformed sessions array in %s\n", source);
            return false;
        }

        char *object_json = (char *)malloc(len + 1);
        if (!object_json) {
            fprintf(stderr, "[native] failed to allocate session config buffer for %s\n", source);
            return false;
        }
        memcpy(object_json, start, len);
        object_json[len] = '\0';

        bool ok = true;
        int slot = session_slot_from_object(object_json, index);
        if (slot >= 0) {
            ok = apply_session_json(&settings->sessions[slot], object_json, source);
        } else {
            fprintf(stderr, "[native] ignoring session entry %zu with unknown slot in %s\n", index, source);
        }
        free(object_json);
        if (!ok) {
            return false;
        }
    }
}

bool native_settings_json_has_rdp_key(const char *json) {
    static const char *keys[] = {"sessions", "host",      "username",           "password",
                                 "domain",   "port",      "width",              "height",
                                 "fps",      "wheelStep", "wheelScrollDivisor", "audioPrebufferMs",
                                 "audioCodec"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (native_json_find_value(json, keys[i])) {
            return true;
        }
    }
    return false;
}

bool native_settings_apply_json(NativeSettings *settings, const char *json, const char *source) {
    NativeSettings updated = *settings;

    const char *sessions_value = native_json_find_value(json, "sessions");
    if (sessions_value && *sessions_value != '[') {
        /* "sessions" present but not an array (typo, null, truncated document): treat as
         * malformed rather than silently falling back to the legacy parser — a corrupt
         * higher-priority persisted file must not mask valid lower-priority candidates. */
        fprintf(stderr, "[native] malformed 'sessions' value (not an array) in %s\n", source);
        return false;
    }
    if (sessions_value) {
        /* v2: per-slot objects + top-level globals. Top-level legacy keys are ignored on
         * purpose — the flat lookup is substring-based and would otherwise read the first
         * slot object's fields. */
        if (!apply_sessions_array(&updated, json, source) || !apply_global_json(&updated, json, source)) {
            return false;
        }
    } else {
        /* Legacy flat object: single session -> green slot. */
        NativeSessionConfig *green = &updated.sessions[NATIVE_SESSION_SLOT_GREEN];
        if (!(apply_session_json(green, json, source) &&
              apply_json_u16(json, "width", 1, UINT16_MAX, &updated.width, source) &&
              apply_json_u16(json, "height", 1, UINT16_MAX, &updated.height, source) &&
              apply_global_json(&updated, json, source))) {
            return false;
        }
    }

    *settings = updated;
    return true;
}

static bool write_json_string(FILE *file, const char *value) {
    if (fputc('"', file) == EOF) {
        return false;
    }
    if (!value) {
        value = "";
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
        case '"':
            if (fputs("\\\"", file) == EOF) {
                return false;
            }
            break;
        case '\\':
            if (fputs("\\\\", file) == EOF) {
                return false;
            }
            break;
        case '\b':
            if (fputs("\\b", file) == EOF) {
                return false;
            }
            break;
        case '\f':
            if (fputs("\\f", file) == EOF) {
                return false;
            }
            break;
        case '\n':
            if (fputs("\\n", file) == EOF) {
                return false;
            }
            break;
        case '\r':
            if (fputs("\\r", file) == EOF) {
                return false;
            }
            break;
        case '\t':
            if (fputs("\\t", file) == EOF) {
                return false;
            }
            break;
        default:
            if (*p < 0x20) {
                if (fprintf(file, "\\u%04x", (unsigned)*p) < 0) {
                    return false;
                }
            } else if (fputc((int)*p, file) == EOF) {
                return false;
            }
            break;
        }
    }
    return fputc('"', file) != EOF;
}

static bool write_session_json(const NativeSessionConfig *session, int slot, FILE *file) {
    return fprintf(file, "    { \"slot\": \"%s\", \"host\": ", native_session_slot_name(slot)) >= 0 &&
           write_json_string(file, session->host) && fprintf(file, ", \"port\": %u, \"username\": ", (unsigned)session->port) >= 0 &&
           write_json_string(file, session->username) && fprintf(file, ", \"password\": ") >= 0 &&
           write_json_string(file, session->password) && fprintf(file, ", \"domain\": ") >= 0 &&
           write_json_string(file, session->domain) && fprintf(file, ", \"fps\": %u }", (unsigned)session->fps) >= 0;
}

bool native_settings_write_json(const NativeSettings *settings, FILE *file) {
    if (fprintf(file, "{\n  \"sessions\": [\n") < 0) {
        return false;
    }
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (!write_session_json(&settings->sessions[slot], slot, file)) {
            return false;
        }
        if (fprintf(file, "%s\n", slot + 1 < NATIVE_SETTINGS_MAX_SESSIONS ? "," : "") < 0) {
            return false;
        }
    }
    return fprintf(file,
                   "  ],\n  \"wheelStep\": %u,\n  \"wheelScrollDivisor\": %u,\n"
                   "  \"audioCodec\": \"%s\"\n}\n",
                   (unsigned)settings->wheel_step, (unsigned)settings->wheel_scroll_divisor,
                   settings->audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto") >= 0;
}

bool native_settings_save_file(const NativeSettings *settings, const char *path) {
    char temp_path[NATIVE_PERSISTED_CONFIG_PATH_MAX + 16u];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(temp_path)) {
        fprintf(stderr, "[native] persisted config path is too long\n");
        return false;
    }

    /* Create the temp file atomically at 0600 (before any secret is written) and refuse to
     * follow a symlink or reuse an existing file at the predictable .tmp name, so a local
     * attacker can neither read the plaintext password through an open window nor redirect
     * the write to clobber another file. Clear our own stale temp from a prior crash first;
     * O_EXCL|O_NOFOLLOW then fails safely if anyone raced a file/symlink into place. */
    (void)unlink(temp_path);
    int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (temp_fd < 0) {
        fprintf(stderr, "[native] failed to create persisted config temp file %s for write: %s\n", temp_path,
                strerror(errno));
        return false;
    }
    FILE *file = fdopen(temp_fd, "wb");
    if (!file) {
        fprintf(stderr, "[native] failed to open persisted config temp file %s for write: %s\n", temp_path,
                strerror(errno));
        close(temp_fd);
        (void)unlink(temp_path);
        return false;
    }

    bool ok = native_settings_write_json(settings, file);
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(temp_path);
        fprintf(stderr, "[native] failed to write persisted config\n");
        return false;
    }
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        fprintf(stderr, "[native] failed to replace persisted config: %s\n", strerror(errno));
        return false;
    }

    fprintf(stderr, "[native] saved persisted config: %s\n", path);
    return true;
}
