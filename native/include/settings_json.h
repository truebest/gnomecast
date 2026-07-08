#ifndef GNOMECAST_SETTINGS_JSON_H
#define GNOMECAST_SETTINGS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Application settings model + hand-rolled JSON (de)serialization, split out of main.c so
 * the multi-session config logic is host-testable. Two persisted formats are understood:
 *
 *   v2 (written):  { "sessions": [ { "slot": "green", "host": ..., "port": n, "username": ...,
 *                    "password": ..., "domain": ..., "fps": n }, { "slot": "yellow", ... } ],
 *                    "wheelStep": n, "wheelScrollDivisor": n, "audioPrebufferMs": n }
 *   legacy (read): the old flat single-session object (host/port/username/password/domain/
 *                  fps/wheelStep/...) — applied to the green slot.
 *
 * Launch parameters, CLI flags and config.local.json keep the legacy flat shape and target
 * the green slot through the same native_settings_apply_json entry point. */

#define NATIVE_SETTINGS_STRING_MAX 512u
/* Remote color-button slots, in the remote's own button order: red, green, yellow, blue.
 * All four color keys are sessions; app exit is system-driven (webOS EXIT/home). */
#define NATIVE_SETTINGS_MAX_SESSIONS 4

#define NATIVE_SESSION_SLOT_RED 0
#define NATIVE_SESSION_SLOT_GREEN 1
#define NATIVE_SESSION_SLOT_YELLOW 2
#define NATIVE_SESSION_SLOT_BLUE 3

typedef struct NativeSessionConfig {
    char host[NATIVE_SETTINGS_STRING_MAX];
    char username[NATIVE_SETTINGS_STRING_MAX];
    char password[NATIVE_SETTINGS_STRING_MAX];
    char domain[NATIVE_SETTINGS_STRING_MAX];
    uint16_t port;
    uint16_t fps;
} NativeSessionConfig;

/* Global audio codec preference ("audioCodec" JSON key). AUTO advertises Opus+PCM and
 * the server picks Opus (~96kbps, in-process decode); PCM advertises PCM only for a
 * lossless stream (~1.4Mbps per session). Global, not per-slot: the mix runs at one
 * sample rate (Opus decodes at 48kHz, grd's PCM is 44.1kHz). */
#define NATIVE_AUDIO_CODEC_AUTO 0
#define NATIVE_AUDIO_CODEC_PCM 1

typedef struct NativeSettings {
    NativeSessionConfig sessions[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Initial desktop hint; runtime-only (never persisted, forced to 1920x1080 on connect). */
    uint16_t width;
    uint16_t height;
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    uint16_t audio_prebuffer_ms;
    uint16_t audio_codec; /* NATIVE_AUDIO_CODEC_* */
} NativeSettings;

/* Human-facing slot name ("green"/"yellow"); "?" for out-of-range indices. */
const char *native_session_slot_name(int slot);

void native_settings_defaults(NativeSettings *settings);

/* Low-level JSON helpers (shared with main.c's launch-parameter handling).
 * native_json_read_* return 1 = read, 0 = key absent, -1 = present but invalid. */
const char *native_json_skip_ws(const char *p);
const char *native_json_find_value(const char *json, const char *key);
int native_json_read_string(const char *json, const char *key, char *out, size_t cap);
int native_json_read_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *out);
int native_json_read_bool(const char *json, const char *key, bool *out);

/* True when the JSON contains any recognized settings key (legacy flat or "sessions"). */
bool native_settings_json_has_rdp_key(const char *json);

/* Applies a JSON document over *settings (unknown keys ignored, absent keys keep their
 * current values). Auto-detects the v2 "sessions" array vs the legacy flat object; flat
 * documents apply to the green slot. Returns false (settings untouched) on malformed
 * values; `source` is used for error logging. */
bool native_settings_apply_json(NativeSettings *settings, const char *json, const char *source);

/* Serializes v2 JSON to an open stream. Returns false on write error. */
bool native_settings_write_json(const NativeSettings *settings, FILE *file);

/* Atomic save (0600 temp file + rename), mirroring the old persisted-config writer. */
bool native_settings_save_file(const NativeSettings *settings, const char *path);

#endif
