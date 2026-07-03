#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
#include <SDL.h>
#if defined(__has_include)
#if __has_include(<SDL_webOS.h>)
#include <SDL_webOS.h>
#define HELLOLG_HAVE_SDL_WEBOS_CURSOR 1
#endif
#endif
#ifndef HELLOLG_HAVE_SDL_WEBOS_CURSOR
#define HELLOLG_HAVE_SDL_WEBOS_CURSOR 0
#endif
#endif

#include "input_sdl.h"
#include "rdp_ffi.h"
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
#include "ui_preconnect.h"
#else
typedef void NativePreconnectUi;
#endif
#include "audio_ss4s.h"
#include "media_ss4s.h"
#include "video_rgba_sdl.h"
#include "video_ss4s.h"

#define NATIVE_RDP_INITIAL_DESKTOP_WIDTH 1920u
#define NATIVE_RDP_INITIAL_DESKTOP_HEIGHT 1080u
/* webOS always renders the app's graphics/UI plane on a virtual ~1920x1080 logical canvas
 * that the platform scales to the panel; the video decoder plane is independent and can run
 * at the panel's true native resolution regardless of this. There is no benefit to a locally
 * larger SDL surface, so this is fixed rather than user-selectable. */
#define NATIVE_LOCAL_SURFACE_WIDTH 1920
#define NATIVE_LOCAL_SURFACE_HEIGHT 1080

#define NATIVE_CONFIG_PATH "native/config.local.json"
#define NATIVE_CONFIG_MAX_FILE (16u * 1024u)
#define NATIVE_CONFIG_STRING_MAX 512u
#define NATIVE_PERSISTED_CONFIG_FILENAME "settings.json"
#define NATIVE_PERSISTED_CONFIG_PATH_MAX 1024u
#define NATIVE_WHEEL_STEP_DEFAULT 60
#define NATIVE_WHEEL_SCROLL_DIVISOR_DEFAULT 1

typedef struct NativeConfig {
    char host[NATIVE_CONFIG_STRING_MAX];
    char username[NATIVE_CONFIG_STRING_MAX];
    char password[NATIVE_CONFIG_STRING_MAX];
    char domain[NATIVE_CONFIG_STRING_MAX];
    uint16_t port;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    bool relative_mouse;
} NativeConfig;

typedef struct App {
    RdpSession *rdp;
    NativeRgbaSurface *rgba;
    /* Shared SS4S library/player owner; video and audio attach tracks to it. Guarded by
     * video_lock like the tracks themselves. */
    NativeMedia *media;
    NativeVideo *video;
    NativeAudio *audio;
    /* Last negotiated audio format (video_lock), kept for reopening the audio track after
     * a pipeline unload: on the webOS ss4s backends closing EITHER track unloads the
     * whole shared pipeline, so the surviving track must be re-attached. */
    uint32_t audio_codec;
    uint32_t audio_sample_rate;
    uint16_t audio_channels;
    pthread_mutex_t video_lock;
    NativeInput input;
    uint16_t desktop_width;
    uint16_t desktop_height;
    uint16_t target_fps;
    int decoder_errors;
    bool decoder_keyframe_pending;
    /* Set once the SDL graphics layer has presented a single transparent frame so the
     * ss4s hardware video plane underneath shows through. Re-presenting a transparent
     * frame every loop tick raced the video plane's own buffer swaps and produced
     * visible flicker, so this latches to "present once, then leave the window alone". */
    bool video_plane_punched;
    const char *password_for_redaction;
    atomic_bool running;
    atomic_bool session_failed;
    atomic_int exit_code;
    atomic_int current_state;
    atomic_int terminal_state;
    bool interactive_ui;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    bool suppress_next_ascii_text;
    uint32_t suppress_ascii_text_until;
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    int wheel_accumulator;
    /* Only ever written by the SDL thread (mouse motion/button handlers, and the
     * pointer-clamp drain below), so plain last-writer-wins races with a second writer
     * can't happen; atomic only for torn-access safety against the RDP worker thread's
     * reads via native_send_scaled_wheel()/native_update_pointer_window_size(). */
    atomic_int virtual_mouse_x;
    atomic_int virtual_mouse_y;
    /* Set by the RDP worker thread (on_desktop_size, on a RDPGFX_RESET_GRAPHICS_PDU) to ask
     * the SDL thread to re-clamp virtual_mouse_x/y into the new window bounds on its next
     * loop tick, keeping the SDL thread the sole writer of virtual_mouse_x/y. */
    atomic_bool pointer_clamp_pending;
    atomic_uint render_width;
    atomic_uint render_height;
    bool relative_mouse;
    bool relative_mouse_active;
    /* Set (under video_lock) by RDP worker-thread callbacks that need to drop the RGBA
     * surface's SDL texture but don't own the renderer's thread; drained and actually
     * destroyed on the SDL thread in native_present_rgba_frame()/native_stop_rdp(). */
    SDL_Texture *pending_texture_destroy;
#endif
} App;

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static void native_update_pointer_window_size(App *app);
static void native_request_pointer_window_size_update(App *app);
#endif

static void native_reopen_audio_locked(App *app);

static bool copy_config_string(char *dest, size_t cap, const char *value, const char *field) {
    if (!dest || cap == 0) {
        return false;
    }
    if (!value) {
        value = "";
    }
    size_t len = strlen(value);
    if (len >= cap) {
        fprintf(stderr, "[native] config field %s is too long\n", field);
        return false;
    }
    memcpy(dest, value, len + 1);
    return true;
}

static void native_config_defaults(NativeConfig *config) {
    memset(config, 0, sizeof(*config));
    (void)copy_config_string(config->host, sizeof(config->host), "127.0.0.1", "host");
    config->port = 3389;
    config->width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH;
    config->height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT;
    config->fps = 60;
    config->wheel_step = NATIVE_WHEEL_STEP_DEFAULT;
    config->wheel_scroll_divisor = NATIVE_WHEEL_SCROLL_DIVISOR_DEFAULT;
}

static void native_prepare_webos_environment(void) {
    if (!getenv("EGL_PLATFORM") && setenv("EGL_PLATFORM", "wayland", 0) != 0) {
        fprintf(stderr, "[native] failed to set EGL_PLATFORM=wayland: %s\n", strerror(errno));
    }
    if (!getenv("XDG_RUNTIME_DIR") && setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0) != 0) {
        fprintf(stderr, "[native] failed to set XDG_RUNTIME_DIR=/tmp/xdg: %s\n", strerror(errno));
    }
    fprintf(stderr, "[native] webOS env APPID=%s EGL_PLATFORM=%s XDG_RUNTIME_DIR=%s\n", getenv("APPID") ? getenv("APPID") : "(unset)",
            getenv("EGL_PLATFORM") ? getenv("EGL_PLATFORM") : "(unset)",
            getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
}

static void native_prepare_webos_logging(void) {
    const char *path = getenv("HELLOLG_NATIVE_LOG_PATH");
    if (!path || !path[0]) {
        path = "/tmp/gnomecast-native.log";
    }
    if (freopen(path, "w", stderr)) {
        setvbuf(stderr, NULL, _IOLBF, 0);
        fprintf(stderr, "\n[native] --- launch ---\n");
    }
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static bool g_sdl_runtime_initialized = false;

static int native_prepare_sdl_runtime(void) {
    if (!g_sdl_runtime_initialized) {
        if (SDL_Init(0) != 0) {
            fprintf(stderr, "[native] SDL_Init(0) failed: %s\n", SDL_GetError());
            return 4;
        }
        g_sdl_runtime_initialized = true;
    }

    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "5000");
    SDL_SetHint("SDL_WEBOS_CURSOR_FREQUENCY", "60");
    SDL_SetHint("SDL_WEBOS_CURSOR_CALIBRATION_DISABLE", "true");
    SDL_SetHint("SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES", "0x057e/0x0000");
    return 0;
}

static void native_shutdown_sdl_runtime(void) {
    if (g_sdl_runtime_initialized) {
        SDL_Quit();
        g_sdl_runtime_initialized = false;
    }
}
#else
static int native_prepare_sdl_runtime(void) {
    return 0;
}

static void native_shutdown_sdl_runtime(void) {
}
#endif

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *json_find_value(const char *json, const char *key) {
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return NULL;
    }

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after_key = skip_ws(p + (size_t)n);
        if (*after_key == ':') {
            return skip_ws(after_key + 1);
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

static int json_read_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json_find_value(json, key);
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
                int h0 = hex_value(p[0]);
                int h1 = hex_value(p[1]);
                int h2 = hex_value(p[2]);
                int h3 = hex_value(p[3]);
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

static int json_read_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *out) {
    const char *p = json_find_value(json, key);
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

static int json_read_bool(const char *json, const char *key, bool *out) {
    const char *p = json_find_value(json, key);
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

static bool apply_json_string(const char *json, const char *key, char *dest, size_t cap) {
    int result = json_read_string(json, key, dest, cap);
    if (result < 0) {
        fprintf(stderr, "[native] invalid string value for config field %s\n", key);
        return false;
    }
    return true;
}

static bool apply_json_u16(const char *json, const char *key, uint16_t min_value, uint16_t max_value, uint16_t *dest) {
    uint16_t value = 0;
    int result = json_read_u16(json, key, min_value, max_value, &value);
    if (result < 0) {
        fprintf(stderr, "[native] invalid numeric value for config field %s\n", key);
        return false;
    }
    if (result > 0) {
        *dest = value;
    }
    return true;
}

static bool apply_json_bool(const char *json, const char *key, bool *dest) {
    bool value = false;
    int result = json_read_bool(json, key, &value);
    if (result < 0) {
        fprintf(stderr, "[native] invalid boolean value for config field %s\n", key);
        return false;
    }
    if (result > 0) {
        *dest = value;
    }
    return true;
}

static bool native_config_load_json(NativeConfig *config, const char *json) {
    if (!(apply_json_string(json, "host", config->host, sizeof(config->host)) &&
          apply_json_string(json, "username", config->username, sizeof(config->username)) &&
          apply_json_string(json, "password", config->password, sizeof(config->password)) &&
          apply_json_string(json, "domain", config->domain, sizeof(config->domain)) &&
          apply_json_u16(json, "port", 1, UINT16_MAX, &config->port) &&
          apply_json_u16(json, "width", 1, UINT16_MAX, &config->width) &&
          apply_json_u16(json, "height", 1, UINT16_MAX, &config->height) &&
          apply_json_u16(json, "fps", 1, 240, &config->fps) &&
          apply_json_u16(json, "wheelStep", 1, 120, &config->wheel_step) &&
          apply_json_u16(json, "wheelScrollDivisor", 1, 120, &config->wheel_scroll_divisor) &&
          apply_json_bool(json, "relativeMouse", &config->relative_mouse))) {
        return false;
    }
    return true;
}

static bool native_config_json_has_rdp_key(const char *json) {
    static const char *keys[] = {"host", "username", "password", "domain", "port", "width", "height", "fps",
                                 "wheelStep", "wheelScrollDivisor", "relativeMouse"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (json_find_value(json, keys[i])) {
            return true;
        }
    }
    return false;
}

static bool native_config_apply_json_if_present(NativeConfig *config, const char *json, const char *source, bool *applied) {
    if (!native_config_json_has_rdp_key(json)) {
        return true;
    }
    if (!native_config_load_json(config, json)) {
        fprintf(stderr, "[native] failed to parse %s\n", source);
        return false;
    }
    fprintf(stderr, "[native] loaded %s\n", source);
    *applied = true;
    return true;
}

static bool native_config_load_file_internal(NativeConfig *config, const char *path, bool required, bool log_missing) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (required) {
            fprintf(stderr, "[native] config file not found: %s\n", path);
            return false;
        }
        if (log_missing) {
            fprintf(stderr, "[native] config file not found at %s; using defaults and CLI overrides\n", path);
        }
        return true;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        fprintf(stderr, "[native] failed to inspect config file: %s\n", path);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > NATIVE_CONFIG_MAX_FILE) {
        fclose(file);
        fprintf(stderr, "[native] config file is too large: %s\n", path);
        return false;
    }
    rewind(file);

    char *json = (char *)calloc((size_t)size + 5, 1);
    if (!json) {
        fclose(file);
        fprintf(stderr, "[native] failed to allocate config buffer\n");
        return false;
    }

    size_t read_count = fread(json, 1, (size_t)size, file);
    fclose(file);
    if (read_count != (size_t)size) {
        free(json);
        fprintf(stderr, "[native] failed to read config file: %s\n", path);
        return false;
    }

    bool ok = native_config_load_json(config, json);
    free(json);
    if (!ok) {
        fprintf(stderr, "[native] failed to parse config file: %s\n", path);
        return false;
    }
    fprintf(stderr, "[native] loaded config file: %s\n", path);
    return true;
}

static bool native_config_load_file(NativeConfig *config, const char *path, bool required) {
    return native_config_load_file_internal(config, path, required, true);
}

static bool native_config_write_json_string(FILE *file, const char *value) {
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

static bool native_config_save_json_file(const NativeConfig *config, const char *path) {
    char temp_path[NATIVE_PERSISTED_CONFIG_PATH_MAX + 16u];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(temp_path)) {
        fprintf(stderr, "[native] persisted config path is too long\n");
        return false;
    }

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        fprintf(stderr, "[native] failed to open persisted config for write: %s\n", strerror(errno));
        return false;
    }

    bool ok = fprintf(file, "{\n  \"host\": ") >= 0 && native_config_write_json_string(file, config->host) &&
              fprintf(file, ",\n  \"port\": %u,\n  \"username\": ", (unsigned)config->port) >= 0 &&
              native_config_write_json_string(file, config->username) && fprintf(file, ",\n  \"password\": ") >= 0 &&
              native_config_write_json_string(file, config->password) && fprintf(file, ",\n  \"domain\": ") >= 0 &&
              native_config_write_json_string(file, config->domain) &&
              fprintf(file,
                      ",\n  \"fps\": %u,\n  \"wheelStep\": %u,\n  "
                      "\"wheelScrollDivisor\": %u,\n  \"relativeMouse\": %s\n}\n",
                      (unsigned)config->fps, (unsigned)config->wheel_step, (unsigned)config->wheel_scroll_divisor,
                      config->relative_mouse ? "true" : "false") >= 0;

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

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static bool native_config_get_persisted_path(char *path, size_t cap) {
    char *pref_path = SDL_GetPrefPath("truebest", "gnomecast");
    if (!pref_path) {
        fprintf(stderr, "[native] failed to resolve SDL pref path: %s\n", SDL_GetError());
        return false;
    }

    int n = snprintf(path, cap, "%s%s", pref_path, NATIVE_PERSISTED_CONFIG_FILENAME);
    SDL_free(pref_path);
    if (n <= 0 || (size_t)n >= cap) {
        fprintf(stderr, "[native] persisted config path is too long\n");
        return false;
    }
    return true;
}

static bool native_config_load_persisted(NativeConfig *config, bool force_ignore) {
    const char *ignore = getenv("HELLOLG_IGNORE_SAVED_CONFIG");
    if (force_ignore || (ignore && strcmp(ignore, "1") == 0)) {
        fprintf(stderr, "[native] skipped persisted config because saved settings were disabled for this launch\n");
        return true;
    }

    char path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    if (!native_config_get_persisted_path(path, sizeof(path))) {
        fprintf(stderr, "[native] ignored persisted config because the SDL pref path is unavailable\n");
        return true;
    }

    NativeConfig loaded = *config;
    if (!native_config_load_file_internal(&loaded, path, false, false)) {
        fprintf(stderr, "[native] ignored invalid persisted config: %s\n", path);
        return true;
    }
    *config = loaded;
    return true;
}

static bool native_config_save_persisted(const NativeConfig *config) {
    char path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    if (!native_config_get_persisted_path(path, sizeof(path))) {
        return false;
    }
    return native_config_save_json_file(config, path);
}
#else
static bool native_config_load_persisted(NativeConfig *config, bool force_ignore) {
    (void)config;
    (void)force_ignore;
    return true;
}

static bool native_config_save_persisted(const NativeConfig *config) {
    (void)config;
    return true;
}
#endif

static bool native_config_apply_launch_json_string(NativeConfig *config, const char *json, const char *key, int arg_index,
                                                   bool *arg_applied) {
    const char *value = json_find_value(json, key);
    if (!value || *skip_ws(value) != '"') {
        return true;
    }

    char nested[NATIVE_CONFIG_MAX_FILE];
    int result = json_read_string(json, key, nested, sizeof(nested));
    if (result < 0) {
        fprintf(stderr, "[native] invalid string value for webOS launch field %s\n", key);
        return false;
    }
    if (result == 0) {
        return true;
    }

    const char *nested_json = skip_ws(nested);
    if (!nested_json || nested_json[0] != '{') {
        return true;
    }
    char source[96];
    (void)snprintf(source, sizeof(source), "webOS launch argument %d %s JSON", arg_index, key);
    return native_config_apply_json_if_present(config, nested_json, source, arg_applied);
}

static bool native_config_apply_launch_params(NativeConfig *config, int argc, char **argv) {
    bool saw_launch_json = false;
    bool applied = false;
    for (int i = 1; i < argc; i++) {
        const char *params = skip_ws(argv[i]);
        if (!params || params[0] != '{') {
            continue;
        }

        saw_launch_json = true;
        bool arg_applied = false;
        char source[64];
        (void)snprintf(source, sizeof(source), "webOS launch argument %d", i);
        if (!native_config_apply_json_if_present(config, params, source, &arg_applied) ||
            !native_config_apply_launch_json_string(config, params, "params", i, &arg_applied) ||
            !native_config_apply_launch_json_string(config, params, "launchParams", i, &arg_applied)) {
            return false;
        }
        if (!arg_applied) {
            fprintf(stderr, "[native] webOS launch argument %d has no RDP config keys\n", i);
        }
        applied = applied || arg_applied;
    }
    if (saw_launch_json && !applied) {
        fprintf(stderr, "[native] webOS launch parameters did not override RDP config\n");
    }
    return true;
}

static bool native_config_launch_json_ignores_saved_config(const char *json) {
    bool ignore = false;
    if (!json || json[0] != '{') {
        return false;
    }
    if (json_read_bool(json, "ignoreSavedConfig", &ignore) > 0 && ignore) {
        return true;
    }

    for (const char **key = (const char *[]){"params", "launchParams", NULL}; *key; key++) {
        char nested[NATIVE_CONFIG_MAX_FILE];
        int result = json_read_string(json, *key, nested, sizeof(nested));
        if (result <= 0) {
            continue;
        }
        const char *nested_json = skip_ws(nested);
        if (native_config_launch_json_ignores_saved_config(nested_json)) {
            return true;
        }
    }
    return false;
}

static bool native_config_launch_ignores_saved_config(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *params = skip_ws(argv[i]);
        if (native_config_launch_json_ignores_saved_config(params)) {
            return true;
        }
    }
    return false;
}

static const char *arg_value(int argc, char **argv, const char *name, const char *fallback) {
    size_t n = strlen(name);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], name, n) == 0 && argv[i][n] == '=') {
            return argv[i] + n + 1;
        }
        if (strcmp(argv[i], name) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return fallback;
}

static bool arg_exists(int argc, char **argv, const char *name) {
    return arg_value(argc, argv, name, NULL) != NULL;
}

static bool arg_flag_exists(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool apply_cli_string(int argc, char **argv, const char *arg_name, char *dest, size_t cap, const char *field) {
    const char *value = arg_value(argc, argv, arg_name, NULL);
    if (!value) {
        return true;
    }
    return copy_config_string(dest, cap, value, field);
}

static bool parse_u16_text(const char *text, uint16_t min_value, uint16_t max_value, uint16_t *out) {
    if (!text || !text[0]) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < (unsigned long)min_value || value > (unsigned long)max_value) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool apply_cli_u16(int argc, char **argv, const char *arg_name, uint16_t min_value, uint16_t max_value, uint16_t *dest) {
    const char *value = arg_value(argc, argv, arg_name, NULL);
    if (!value) {
        return true;
    }
    uint16_t parsed = 0;
    if (!parse_u16_text(value, min_value, max_value, &parsed)) {
        fprintf(stderr, "[native] invalid value for %s\n", arg_name);
        return false;
    }
    *dest = parsed;
    return true;
}

static bool native_config_apply_cli(NativeConfig *config, int argc, char **argv) {
    if (arg_flag_exists(argc, argv, "--relative-mouse")) {
        config->relative_mouse = true;
    }
    if (arg_flag_exists(argc, argv, "--absolute-mouse")) {
        config->relative_mouse = false;
    }
    if (!(apply_cli_string(argc, argv, "--host", config->host, sizeof(config->host), "host") &&
          apply_cli_string(argc, argv, "--user", config->username, sizeof(config->username), "username") &&
          apply_cli_string(argc, argv, "--username", config->username, sizeof(config->username), "username") &&
          apply_cli_string(argc, argv, "--password", config->password, sizeof(config->password), "password") &&
          apply_cli_string(argc, argv, "--domain", config->domain, sizeof(config->domain), "domain") &&
          apply_cli_u16(argc, argv, "--port", 1, UINT16_MAX, &config->port) &&
          apply_cli_u16(argc, argv, "--width", 1, UINT16_MAX, &config->width) &&
          apply_cli_u16(argc, argv, "--height", 1, UINT16_MAX, &config->height) &&
          apply_cli_u16(argc, argv, "--fps", 1, 240, &config->fps) &&
          apply_cli_u16(argc, argv, "--wheel-step", 1, 120, &config->wheel_step) &&
          apply_cli_u16(argc, argv, "--wheel-scroll-divisor", 1, 120, &config->wheel_scroll_divisor))) {
        return false;
    }
    return true;
}

static void native_config_apply_initial_desktop_hint(NativeConfig *config) {
    if (!config) {
        return;
    }
    config->width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH;
    config->height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT;
}

static bool native_config_validate_runtime(const NativeConfig *config) {
    bool ok = true;
    if (config->port == 0 || config->width == 0 || config->height == 0 || config->fps == 0 || config->wheel_step == 0 ||
        config->wheel_scroll_divisor == 0) {
        fprintf(stderr, "[native] invalid zero value in RDP config\n");
        ok = false;
    }
    return ok;
}

static bool native_config_validate_connect(const NativeConfig *config, char *message, size_t message_cap) {
    bool ok = true;
    if (!config->host[0]) {
        fprintf(stderr, "[native] missing RDP host in config/CLI\n");
        ok = false;
    }
    if (!config->username[0]) {
        fprintf(stderr, "[native] missing RDP username in config/CLI/launch parameters\n");
        ok = false;
    }
    if (!config->password[0]) {
        fprintf(stderr, "[native] missing RDP password in config/CLI/launch parameters\n");
        ok = false;
    }
    if (!native_config_validate_runtime(config)) {
        ok = false;
    }
    if (!ok && message && message_cap > 0) {
        if (!config->host[0]) {
            (void)snprintf(message, message_cap, "Enter an IP address.");
        } else if (!config->username[0] || !config->password[0]) {
            (void)snprintf(message, message_cap, "Missing username or password in native config.");
        } else {
            (void)snprintf(message, message_cap, "Invalid RDP config.");
        }
    }
    return ok;
}

static bool native_config_validate(const NativeConfig *config) {
    return native_config_validate_connect(config, NULL, 0);
}

static void native_config_log_effective(const NativeConfig *config) {
    fprintf(stderr,
            "[native] effective RDP config host=%s port=%u username=%s password=%s domain=%s desktop=%ux%u@%u wheelStep=%u wheelScrollDivisor=%u\n",
            config->host, (unsigned)config->port, config->username[0] ? "set" : "missing",
            config->password[0] ? "set" : "missing", config->domain[0] ? "set" : "empty", (unsigned)config->width,
            (unsigned)config->height, (unsigned)config->fps, (unsigned)config->wheel_step,
            (unsigned)config->wheel_scroll_divisor);
    fprintf(stderr, "[native] effective mouse mode=%s\n", config->relative_mouse ? "relative" : "absolute");
}

static const char *rdp_state_name(RdpState state) {
    switch (state) {
    case RDP_STATE_IDLE:
        return "Idle";
    case RDP_STATE_CONNECTING:
        return "Connecting";
    case RDP_STATE_TLS:
        return "Tls";
    case RDP_STATE_CREDSSP:
        return "Credssp";
    case RDP_STATE_ACTIVE:
        return "Active";
    case RDP_STATE_NO_AVC420:
        return "NoAvc420";
    case RDP_STATE_DECODER_ERROR:
        return "DecoderError";
    case RDP_STATE_NETWORK_ERROR:
        return "NetworkError";
    case RDP_STATE_PROTOCOL_ERROR:
        return "ProtocolError";
    case RDP_STATE_STOPPED:
        return "Stopped";
    }
    return "Unknown";
}

static bool rdp_state_is_terminal_error(RdpState state) {
    return state == RDP_STATE_NO_AVC420 || state == RDP_STATE_DECODER_ERROR || state == RDP_STATE_NETWORK_ERROR ||
           state == RDP_STATE_PROTOCOL_ERROR;
}

static bool rdp_state_is_native_runtime_failure(RdpState state) {
    return state == RDP_STATE_NO_AVC420 || state == RDP_STATE_DECODER_ERROR;
}

static int rdp_state_exit_code(RdpState state) {
    switch (state) {
    case RDP_STATE_NO_AVC420:
        return 10;
    case RDP_STATE_DECODER_ERROR:
        return 11;
    case RDP_STATE_NETWORK_ERROR:
        return 12;
    case RDP_STATE_PROTOCOL_ERROR:
        return 13;
    default:
        return 0;
    }
}

static const char *redact_if_sensitive(const App *app, const char *line) {
    if (!line) {
        return "";
    }
    if (app && app->password_for_redaction && strlen(app->password_for_redaction) >= 4 &&
        strstr(line, app->password_for_redaction)) {
        return "[redacted sensitive detail]";
    }
    return line;
}

static void app_stop_with_state(App *app, RdpState state, int exit_code) {
    if (!app) {
        return;
    }
    atomic_store(&app->terminal_state, (int)state);
    if (exit_code != 0) {
        atomic_store(&app->exit_code, exit_code);
    }
    if (app->interactive_ui &&
        (state == RDP_STATE_STOPPED || (rdp_state_is_terminal_error(state) && !rdp_state_is_native_runtime_failure(state)))) {
        atomic_store(&app->session_failed, true);
    } else {
        atomic_store(&app->running, false);
    }
}

static void on_state(void *ctx, RdpState state, const char *detail) {
    App *app = (App *)ctx;
    if (app) {
        atomic_store(&app->current_state, (int)state);
        native_input_set_active(&app->input, state == RDP_STATE_ACTIVE);
    }

    const char *safe_detail = redact_if_sensitive(app, detail);
    fprintf(stderr, "[native] state=%s(%d)%s%s\n", rdp_state_name(state), (int)state, safe_detail[0] ? " " : "", safe_detail);

    if (app && rdp_state_is_terminal_error(state)) {
        fprintf(stderr, "[native] terminal native error: %s; no web/rendering fallback will be attempted\n", rdp_state_name(state));
        app_stop_with_state(app, state, rdp_state_exit_code(state));
    } else if (app && state == RDP_STATE_STOPPED && atomic_load(&app->exit_code) == 0) {
        /* app_stop_with_state() itself branches on interactive_ui: interactive builds return
         * gracefully to the pre-connect UI, non-interactive builds exit the process. Gating
         * this call on interactive_ui too meant a clean disconnect never called it at all in
         * non-interactive builds, leaving the SDL loop spinning forever on a stale session. */
        app_stop_with_state(app, state, 0);
    }
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
/* SDL_DestroyTexture() must run on the thread that owns the renderer (the SDL/main thread),
 * but native_rgba_surface_close()/resize() can be invoked from the RDP worker thread
 * (on_bitmap_update, on_video_au). Call this (while holding video_lock) before either, so
 * they find no texture to destroy themselves; the detached texture is destroyed later on
 * the SDL thread, drained in native_present_rgba_frame()/native_stop_rdp(). */
static void native_defer_rgba_texture_destroy(App *app) {
    if (!app || !app->rgba) {
        return;
    }
    SDL_Texture *stale = native_rgba_surface_take_texture(app->rgba);
    if (!stale) {
        return;
    }
    if (app->pending_texture_destroy) {
        fprintf(stderr, "[native] warning: leaking undrained RGBA texture to avoid cross-thread SDL_DestroyTexture\n");
    }
    app->pending_texture_destroy = stale;
}
#endif

static void on_bitmap_update(void *ctx, uint16_t surface_id, uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                             uint32_t stride, const uint8_t *rgba, size_t len) {
    App *app = (App *)ctx;
    if (!app) {
        return;
    }
    if (!rgba || len == 0 || width == 0 || height == 0) {
        app->decoder_errors++;
        app_stop_with_state(app, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }

    pthread_mutex_lock(&app->video_lock);
    if (app->video) {
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
        fprintf(stderr, "[native] switching graphics path from ss4s/H.264 to native RemoteFX RGBA\n");
        /* Closing the video track unloaded the shared pipeline; bring audio back. */
        native_reopen_audio_locked(app);
    }
    if (!app->rgba) {
        app->rgba = native_rgba_surface_open(app->desktop_width, app->desktop_height);
    } else if (native_rgba_surface_width(app->rgba) != app->desktop_width ||
               native_rgba_surface_height(app->rgba) != app->desktop_height) {
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
        native_defer_rgba_texture_destroy(app);
#endif
        if (native_rgba_surface_resize(app->rgba, app->desktop_width, app->desktop_height) != NATIVE_RGBA_OK) {
            native_rgba_surface_close(app->rgba);
            app->rgba = NULL;
        }
    }
    NativeRgbaResult result = app->rgba ? native_rgba_surface_apply(app->rgba, left, top, width, height, stride, rgba, len)
                                        : NATIVE_RGBA_NOMEM;
    pthread_mutex_unlock(&app->video_lock);

    if (result != NATIVE_RGBA_OK) {
        app->decoder_errors++;
        fprintf(stderr,
                "[native] terminal native error: DecoderError; RemoteFX bitmap update failed result=%d surface=%u rect=%ux%u+%u+%u\n",
                (int)result, (unsigned)surface_id, (unsigned)width, (unsigned)height, (unsigned)left, (unsigned)top);
        app_stop_with_state(app, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
}

static void on_log(void *ctx, const char *line) {
    App *app = (App *)ctx;
    fprintf(stderr, "[rdp] %s\n", redact_if_sensitive(app, line));
}

/* Fires on the initial MCS/GCC handshake and again on every RDPGFX_RESET_GRAPHICS_PDU, so
 * the real EGFX graphics output size (which can differ from the negotiated session size)
 * stays current for both the ss4s/H.264 and RemoteFX RGBA paths and for pointer mapping. */
static void on_desktop_size(void *ctx, uint16_t width, uint16_t height) {
    App *app = (App *)ctx;
    if (!app) {
        return;
    }
    app->desktop_width = width;
    app->desktop_height = height;
    native_input_set_desktop_size(&app->input, width, height);
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    /* Called on the RDP worker thread; defer the pointer re-clamp to the SDL thread
     * (native_drain_pointer_clamp in the main loop) so it stays the sole writer of
     * virtual_mouse_x/y. */
    native_request_pointer_window_size_update(app);
#endif
    fprintf(stderr, "[native] desktop=%ux%u\n", (unsigned)width, (unsigned)height);
}

/* Creates the shared SS4S media owner on first use. Caller must hold app->video_lock. */
static NativeMedia *native_ensure_media_locked(App *app) {
    if (!app) {
        return NULL;
    }
    if (app->media) {
        return app->media;
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    uint16_t viewport_width = (uint16_t)atomic_load(&app->render_width);
    uint16_t viewport_height = (uint16_t)atomic_load(&app->render_height);
#else
    uint16_t viewport_width = (uint16_t)atomic_load(&app->input.window_width);
    uint16_t viewport_height = (uint16_t)atomic_load(&app->input.window_height);
#endif
    /* Same defaulting the old open-at-first-AU path used when no render size is known
     * yet (e.g. audio negotiates before the first video frame). */
    if (viewport_width == 0) {
        viewport_width = app->desktop_width;
    }
    if (viewport_height == 0) {
        viewport_height = app->desktop_height;
    }
    app->media = native_media_open(viewport_width, viewport_height);
    return app->media;
}

/* Opens the audio track speculatively as Opus 48kHz stereo before the rdpsnd negotiation
 * confirms it. The outcome is deterministic for servers that offer Opus (the client
 * advertises [Opus 48k, PCM] and gnome-remote-desktop picks by its own priority from the
 * client's list), and opening audio EARLY removes the track-open race entirely: both
 * tracks share one webOS hardware pipeline, so an audio open landing after the video
 * stream has started reloads the pipeline and stalls video until an IDR the server never
 * resends. If negotiation ends up choosing something else (or the server has no audio),
 * the normal on_audio_format path corrects it. Caller must hold app->video_lock. */
static void native_open_speculative_audio_locked(App *app) {
    if (!app || app->audio || !app->media) {
        return;
    }
    app->audio = native_audio_open(app->media, RDP_AUDIO_CODEC_OPUS, 48000, 2);
    if (app->audio) {
        app->audio_codec = RDP_AUDIO_CODEC_OPUS;
        app->audio_sample_rate = 48000;
        app->audio_channels = 2;
        fprintf(stderr, "[native-audio] opened speculative OPUS 48000Hz 2ch track ahead of negotiation\n");
    }
}

/* Reopens the audio track after something unloaded the shared media pipeline (on the
 * webOS ss4s backends closing either track does that). Cheap for audio: Opus/PCM need no
 * keyframe, so sound resumes right after the reload. Caller must hold app->video_lock. */
static void native_reopen_audio_locked(App *app) {
    if (!app || !app->audio || !app->media) {
        return;
    }
    native_audio_close(app->audio);
    app->audio = native_audio_open(app->media, app->audio_codec, app->audio_sample_rate, app->audio_channels);
    if (!app->audio) {
        fprintf(stderr, "[native-audio] failed to reopen audio after pipeline reload; continuing without audio\n");
    }
}

static void on_video_au(void *ctx, const uint8_t *data, size_t len, bool is_keyframe, uint64_t pts90k) {
    App *app = (App *)ctx;
    if (!app) {
        return;
    }
    if (!data || len == 0) {
        app->decoder_errors++;
        app_stop_with_state(app, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }

    /* app->desktop_width/height reflect the server's real EGFX graphics output size
     * (on_desktop_size is re-invoked on every RDPGFX_RESET_GRAPHICS_PDU, not just the
     * initial MCS/GCC handshake), which can differ from the negotiated session size, e.g.
     * a TV whose hardware decoder always runs at panel resolution. Reopen the ss4s decoder
     * whenever that size changes so it matches what the server actually encodes. */
    pthread_mutex_lock(&app->video_lock);
    if (app->rgba) {
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
        native_defer_rgba_texture_destroy(app);
#endif
        native_rgba_surface_close(app->rgba);
        app->rgba = NULL;
        fprintf(stderr, "[native] switching graphics path from native RemoteFX RGBA to ss4s/H.264\n");
    }
    if (app->video &&
        (app->desktop_width != native_video_width(app->video) || app->desktop_height != native_video_height(app->video))) {
        fprintf(stderr, "[native] ss4s surface size changed %ux%u -> %ux%u; reopening decoder\n",
                (unsigned)native_video_width(app->video), (unsigned)native_video_height(app->video),
                (unsigned)app->desktop_width, (unsigned)app->desktop_height);
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
    }
    if (!app->video) {
        NativeMedia *media = native_ensure_media_locked(app);
        if (media) {
            /* Audio must attach before the video track: the video open below triggers
             * the single pipeline load with the keyframe already in hand, whereas an
             * audio open arriving later would reload the pipeline mid-stream. */
            native_open_speculative_audio_locked(app);
            app->video = native_video_open(media, app->desktop_width, app->desktop_height, app->target_fps);
        }
        if (!app->video) {
            pthread_mutex_unlock(&app->video_lock);
            app->decoder_errors++;
            fprintf(stderr, "[native] terminal native error: DecoderError; ss4s decoder unavailable\n");
            app_stop_with_state(app, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
            return;
        }
    }

    /* The callback byte lifetime is synchronous: do not retain data beyond native_video_feed.
     * If this callback later queues AUs to another thread, copy the bytes before returning.
     */
    NativeVideoResult result = native_video_feed(app->video, data, len, is_keyframe, pts90k);
    pthread_mutex_unlock(&app->video_lock);
    if (result == NATIVE_VIDEO_OK) {
        app->decoder_keyframe_pending = false;
        return;
    }

    if (result == NATIVE_VIDEO_NEED_KEYFRAME) {
        if (!app->decoder_keyframe_pending) {
            fprintf(stderr,
                    "[native] decoder requested keyframe/recovery; waiting for native RDP recovery, no web fallback\n");
            app->decoder_keyframe_pending = true;
        }
        return;
    }

    app->decoder_errors++;
    fprintf(stderr, "[native] terminal native error: DecoderError; decoder feed result=%d\n", (int)result);
    app_stop_with_state(app, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
}

/* Audio is strictly best-effort: any failure here logs and degrades to silent video;
 * neither handler ever stops the session (and must never call rdp_session_stop, which
 * would self-join the rdp-worker thread these callbacks run on). */
static void on_audio_format(void *ctx, uint32_t codec, uint32_t sample_rate, uint16_t channels) {
    App *app = (App *)ctx;
    if (!app) {
        return;
    }
    pthread_mutex_lock(&app->video_lock);
    /* The expected outcome of the speculative open: negotiation confirmed the format the
     * track is already running with, so don't touch the shared pipeline at all. */
    if (app->audio && app->audio_codec == codec && app->audio_sample_rate == sample_rate &&
        app->audio_channels == channels) {
        fprintf(stderr, "[native-audio] negotiation confirmed the already-open audio format\n");
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    if (app->audio) {
        native_audio_close(app->audio);
        app->audio = NULL;
        /* That close unloaded the shared pipeline, resetting the hardware H.264 decoder
         * mid-stream. Drop the dead video track; on_video_au reopens it on the next
         * SPS+PPS+IDR instead of feeding P-frames into a fresh decoder. */
        if (app->video) {
            fprintf(stderr, "[native] audio format change reloaded the media pipeline; reopening video on next keyframe\n");
            native_video_close(app->video);
            app->video = NULL;
            app->decoder_keyframe_pending = false;
        }
    }
    NativeMedia *media = native_ensure_media_locked(app);
    if (media) {
        app->audio = native_audio_open(media, codec, sample_rate, channels);
    }
    if (app->audio) {
        app->audio_codec = codec;
        app->audio_sample_rate = sample_rate;
        app->audio_channels = channels;
        /* First-time open under a live video stream also reloads the pipeline. */
        if (app->video) {
            fprintf(stderr, "[native] audio open reloaded the media pipeline; reopening video on next keyframe\n");
            native_video_close(app->video);
            app->video = NULL;
            app->decoder_keyframe_pending = false;
            native_reopen_audio_locked(app);
        }
    } else {
        fprintf(stderr, "[native-audio] audio unavailable (codec=%u rate=%u channels=%u); continuing with silent video\n",
                (unsigned)codec, (unsigned)sample_rate, (unsigned)channels);
    }
    pthread_mutex_unlock(&app->video_lock);
}

static void on_audio_data(void *ctx, const uint8_t *data, size_t len, uint32_t ts_ms) {
    (void)ts_ms; /* ss4s derives its own playback clock from feed time. */
    App *app = (App *)ctx;
    if (!app || !data || len == 0) {
        return;
    }
    pthread_mutex_lock(&app->video_lock);
    if (app->audio && native_audio_feed(app->audio, data, len) == NATIVE_AUDIO_ERROR) {
        /* Mute instead of closing: on the webOS ss4s backends closing the audio track
         * unloads the shared pipeline and would freeze a live video stream. */
        fprintf(stderr, "[native-audio] audio feed failed; muting audio for this session\n");
        native_audio_disable(app->audio);
    }
    pthread_mutex_unlock(&app->video_lock);
}

static void native_stop_rdp(App *app) {
    if (!app) {
        return;
    }
    native_input_set_active(&app->input, false);
    native_input_set_session(&app->input, NULL);
    if (app->rdp) {
        rdp_session_stop(app->rdp);
        app->rdp = NULL;
    }
    pthread_mutex_lock(&app->video_lock);
    native_rgba_surface_close(app->rgba);
    app->rgba = NULL;
    /* Tracks first, then the shared player/library they attach to. */
    native_audio_close(app->audio);
    app->audio = NULL;
    native_video_close(app->video);
    app->video = NULL;
    native_media_close(app->media);
    app->media = NULL;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    /* native_stop_rdp() only ever runs on the SDL/main thread, so it's safe to destroy
     * directly here rather than deferring; drain any texture a worker-thread callback
     * handed off but the render loop hasn't gotten to yet. */
    if (app->pending_texture_destroy) {
        SDL_DestroyTexture(app->pending_texture_destroy);
        app->pending_texture_destroy = NULL;
    }
#endif
    pthread_mutex_unlock(&app->video_lock);
    atomic_store(&app->current_state, (int)RDP_STATE_IDLE);
}

static bool native_start_rdp(App *app, const NativeConfig *native_config, const RdpCallbacks *callbacks) {
    if (!app || !native_config || !callbacks) {
        return false;
    }
    native_stop_rdp(app);
    app->desktop_width = native_config->width;
    app->desktop_height = native_config->height;
    app->target_fps = native_config->fps;
    app->decoder_errors = 0;
    app->decoder_keyframe_pending = false;
    app->video_plane_punched = false;
    app->password_for_redaction = native_config->password;
    uint16_t window_width = (uint16_t)atomic_load(&app->input.window_width);
    uint16_t window_height = (uint16_t)atomic_load(&app->input.window_height);
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    app->relative_mouse = native_config->relative_mouse;
#endif
    atomic_store(&app->exit_code, 0);
    atomic_store(&app->session_failed, false);
    atomic_store(&app->terminal_state, (int)RDP_STATE_IDLE);
    atomic_store(&app->current_state, (int)RDP_STATE_IDLE);
    native_input_init(&app->input, NULL, native_config->width, native_config->height);
    if (window_width != 0 && window_height != 0) {
        native_input_set_window_size(&app->input, window_width, window_height);
    }

    RdpConfig config = {
        .host = native_config->host,
        .port = native_config->port,
        .username = native_config->username,
        .password = native_config->password,
        .domain = native_config->domain,
        .width = native_config->width,
        .height = native_config->height,
        .fps = native_config->fps,
    };

    fprintf(stderr, "[native] starting native-only gnomecast for %s:%u (%ux%u@%u AVC420/RemoteFX)\n", config.host,
            (unsigned)config.port, (unsigned)config.width, (unsigned)config.height, (unsigned)config.fps);
    app->rdp = rdp_session_start(&config, callbacks);
    if (!app->rdp) {
        fprintf(stderr, "[native] rdp_session_start failed\n");
        return false;
    }
    native_input_set_session(&app->input, app->rdp);
    return true;
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static uint16_t clamp_sdl_dimension(int value) {
    if (value <= 0) {
        return 1;
    }
    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

static void native_init_virtual_mouse_position(App *app);
static void native_set_virtual_mouse_position(App *app, int x, int y);

static void native_log_sdl_display_modes(void) {
    int display_count = SDL_GetNumVideoDisplays();
    if (display_count <= 0) {
        fprintf(stderr, "[native] SDL video displays unavailable: %s\n", SDL_GetError());
        return;
    }

    fprintf(stderr, "[native] SDL video displays=%d\n", display_count);
    for (int display = 0; display < display_count; display++) {
        SDL_DisplayMode desktop_mode;
        if (SDL_GetDesktopDisplayMode(display, &desktop_mode) == 0) {
            fprintf(stderr, "[native] SDL display %d desktop=%dx%d@%d format=0x%x\n", display, desktop_mode.w,
                    desktop_mode.h, desktop_mode.refresh_rate, (unsigned)desktop_mode.format);
        } else {
            fprintf(stderr, "[native] SDL display %d desktop mode unavailable: %s\n", display, SDL_GetError());
        }

        SDL_DisplayMode current_mode;
        if (SDL_GetCurrentDisplayMode(display, &current_mode) == 0) {
            fprintf(stderr, "[native] SDL display %d current=%dx%d@%d format=0x%x\n", display, current_mode.w,
                    current_mode.h, current_mode.refresh_rate, (unsigned)current_mode.format);
        } else {
            fprintf(stderr, "[native] SDL display %d current mode unavailable: %s\n", display, SDL_GetError());
        }

        int mode_count = SDL_GetNumDisplayModes(display);
        if (mode_count < 0) {
            fprintf(stderr, "[native] SDL display %d modes unavailable: %s\n", display, SDL_GetError());
            continue;
        }
        fprintf(stderr, "[native] SDL display %d modes=%d\n", display, mode_count);
        for (int mode_index = 0; mode_index < mode_count; mode_index++) {
            SDL_DisplayMode mode;
            if (SDL_GetDisplayMode(display, mode_index, &mode) == 0) {
                fprintf(stderr, "[native] SDL display %d mode %d=%dx%d@%d format=0x%x\n", display, mode_index,
                        mode.w, mode.h, mode.refresh_rate, (unsigned)mode.format);
            }
        }
    }
}

static bool native_read_renderer_output_size(SDL_Renderer *renderer, int *width, int *height) {
    if (!renderer) {
        return false;
    }
    int output_width = 0;
    int output_height = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, &output_height) != 0 || output_width <= 0 || output_height <= 0) {
        fprintf(stderr, "[native] SDL renderer output size unavailable: %s\n", SDL_GetError());
        return false;
    }
    if (width) {
        *width = output_width;
    }
    if (height) {
        *height = output_height;
    }
    return true;
}

static bool native_update_render_size(App *app, SDL_Renderer *renderer) {
    int render_width = 0;
    int render_height = 0;
    if (!native_read_renderer_output_size(renderer, &render_width, &render_height)) {
        return false;
    }
    uint16_t clamped_width = clamp_sdl_dimension(render_width);
    uint16_t clamped_height = clamp_sdl_dimension(render_height);
    if (app && (atomic_load(&app->render_width) != clamped_width || atomic_load(&app->render_height) != clamped_height)) {
        atomic_store(&app->render_width, clamped_width);
        atomic_store(&app->render_height, clamped_height);
        fprintf(stderr, "[native] SDL renderer output=%dx%d\n", render_width, render_height);
        pthread_mutex_lock(&app->video_lock);
        native_media_set_viewport(app->media, clamped_width, clamped_height);
        pthread_mutex_unlock(&app->video_lock);
    }
    return true;
}

static void native_sync_input_window_size(App *app) {
    if (!app) {
        return;
    }
    uint16_t render_width = (uint16_t)atomic_load(&app->render_width);
    uint16_t render_height = (uint16_t)atomic_load(&app->render_height);
    uint16_t width = render_width ? render_width : (uint16_t)atomic_load(&app->input.window_width);
    uint16_t height = render_height ? render_height : (uint16_t)atomic_load(&app->input.window_height);
    native_input_set_window_size(&app->input, width, height);
}

/* SDL-thread-only: updates the window-size mapping and immediately re-clamps
 * virtual_mouse_x/y into the new bounds. */
static void native_update_pointer_window_size(App *app) {
    native_sync_input_window_size(app);
    if (!app) {
        return;
    }
    native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
}

/* RDP-worker-thread-safe counterpart of native_update_pointer_window_size(): updates the
 * window-size mapping directly (native_input's fields are themselves atomic), but leaves
 * the actual virtual_mouse_x/y re-clamp to the SDL thread's next loop tick instead of
 * writing it here, so the SDL thread stays the sole writer of virtual_mouse_x/y. */
static void native_request_pointer_window_size_update(App *app) {
    native_sync_input_window_size(app);
    if (!app) {
        return;
    }
    atomic_store(&app->pointer_clamp_pending, true);
}

static void native_drain_pointer_clamp(App *app) {
    if (!app) {
        return;
    }
    if (atomic_exchange(&app->pointer_clamp_pending, false)) {
        native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
    }
}

static bool native_init_render_state(App *app, SDL_Window *window, SDL_Renderer *renderer, int *window_width,
                                     int *window_height, char *message, size_t message_cap) {
    int current_width = 0;
    int current_height = 0;
    SDL_GetWindowSize(window, &current_width, &current_height);
    fprintf(stderr, "[native] SDL window size=%dx%d\n", current_width, current_height);
    if (!native_update_render_size(app, renderer)) {
        if (message && message_cap > 0) {
            (void)snprintf(message, message_cap, "Failed to read local SDL render target.");
        }
        return false;
    }
    if (window_width) {
        *window_width = current_width;
    }
    if (window_height) {
        *window_height = current_height;
    }
    native_update_pointer_window_size(app);
    native_init_virtual_mouse_position(app);
    return true;
}

static NativeInputButton native_button_from_sdl(uint8_t button);

static void native_log_webos_cursor_visibility(bool visible, int result) {
    static unsigned log_count = 0;
    if (log_count < 16) {
        if (result == 0) {
            fprintf(stderr, "[native-input] webOS cursor visibility=%d\n", visible ? 1 : 0);
        } else {
            fprintf(stderr, "[native-input] SDL_webOSCursorVisibility(%d) failed: %s\n", visible ? 1 : 0, SDL_GetError());
        }
    } else if (log_count == 16) {
        fprintf(stderr, "[native-input] further webOS cursor visibility logs suppressed\n");
    }
    log_count++;
}

static bool native_sdl_key_is_webos_cursor_event(const SDL_KeyboardEvent *event) {
    if (!event) {
        return false;
    }
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    return event->keysym.scancode == SDL_WEBOS_SCANCODE_CURSOR_SHOW ||
           event->keysym.scancode == SDL_WEBOS_SCANCODE_CURSOR_HIDE;
#else
    return event->keysym.scancode == 484 || event->keysym.scancode == 485;
#endif
}

static bool native_sdl_key_is_webos_red_event(const SDL_KeyboardEvent *event) {
    if (!event) {
        return false;
    }
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    return event->keysym.scancode == SDL_WEBOS_SCANCODE_RED || event->keysym.scancode == 486;
#else
    return event->keysym.scancode == 486;
#endif
}

static bool native_handle_webos_red_key(App *app, const SDL_KeyboardEvent *event) {
    if (!app || !event || event->type != SDL_KEYDOWN || !native_sdl_key_is_webos_red_event(event)) {
        return false;
    }
    fprintf(stderr, "[native] webOS red key requests shutdown\n");
    atomic_store(&app->running, false);
    return true;
}

static int native_filter_webos_red_key(void *userdata, SDL_Event *event) {
    App *app = (App *)userdata;
    if (!event || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) ||
        !native_sdl_key_is_webos_red_event(&event->key)) {
        return 1;
    }
    if (event->type == SDL_KEYDOWN) {
        (void)native_handle_webos_red_key(app, &event->key);
    }
    return 0;
}

static void native_hide_webos_cursor_for_key(SDL_Window *window, const SDL_KeyboardEvent *event) {
    if (!event || event->type != SDL_KEYDOWN || native_sdl_key_is_webos_cursor_event(event)) {
        return;
    }
    if (window && SDL_IsScreenKeyboardShown(window)) {
        return;
    }
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    int result = SDL_webOSCursorVisibility(SDL_FALSE);
    native_log_webos_cursor_visibility(false, result);
#else
    static bool logged = false;
    if (!logged) {
        fprintf(stderr, "[native-input] SDL_webOSCursorVisibility is not available in this SDL build\n");
        logged = true;
    }
#endif
}

static void native_send_scaled_wheel(App *app, int wheel_y) {
    if (!app || wheel_y == 0) {
        return;
    }

    int units = wheel_y * (int)app->wheel_step;
    if ((app->wheel_accumulator > 0 && units < 0) || (app->wheel_accumulator < 0 && units > 0)) {
        app->wheel_accumulator = 0;
    }
    app->wheel_accumulator += units;

    const int threshold = (int)app->wheel_step * (int)app->wheel_scroll_divisor;
    int mouse_x = atomic_load(&app->virtual_mouse_x);
    int mouse_y = atomic_load(&app->virtual_mouse_y);
    while (app->wheel_accumulator >= threshold) {
        native_input_pointer_wheel(&app->input, mouse_x, mouse_y, (int16_t)app->wheel_step);
        app->wheel_accumulator -= threshold;
    }
    while (app->wheel_accumulator <= -threshold) {
        native_input_pointer_wheel(&app->input, mouse_x, mouse_y, (int16_t)-app->wheel_step);
        app->wheel_accumulator += threshold;
    }
}

static int native_clamp_window_coord(int value, uint16_t limit) {
    if (value < 0) {
        return 0;
    }
    if (limit == 0) {
        return 0;
    }
    if (value >= (int)limit) {
        return (int)limit - 1;
    }
    return value;
}

static void native_set_virtual_mouse_position(App *app, int x, int y) {
    if (!app) {
        return;
    }
    atomic_store(&app->virtual_mouse_x, native_clamp_window_coord(x, (uint16_t)atomic_load(&app->input.window_width)));
    atomic_store(&app->virtual_mouse_y, native_clamp_window_coord(y, (uint16_t)atomic_load(&app->input.window_height)));
}

static void native_init_virtual_mouse_position(App *app) {
    if (!app) {
        return;
    }
    native_set_virtual_mouse_position(app, (int)atomic_load(&app->input.window_width) / 2,
                                       (int)atomic_load(&app->input.window_height) / 2);
}

static void native_set_streaming_mouse_mode(App *app, bool enabled) {
    if (!app) {
        return;
    }
    SDL_bool relative = enabled && app->relative_mouse ? SDL_TRUE : SDL_FALSE;
    app->relative_mouse_active = false;
    if (SDL_SetRelativeMouseMode(relative) != 0) {
        fprintf(stderr, "[native-input] SDL_SetRelativeMouseMode(%d) failed: %s\n", relative == SDL_TRUE ? 1 : 0,
                SDL_GetError());
    } else {
        app->relative_mouse_active = relative == SDL_TRUE;
        fprintf(stderr, "[native-input] mouse mode=%s\n", relative == SDL_TRUE ? "relative" : "absolute");
    }
}

static NativeInputButton native_button_from_sdl(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return NATIVE_INPUT_BUTTON_LEFT;
    case SDL_BUTTON_RIGHT:
        return NATIVE_INPUT_BUTTON_RIGHT;
    case SDL_BUTTON_MIDDLE:
        return NATIVE_INPUT_BUTTON_MIDDLE;
    default:
        return 0;
    }
}

static bool sdl_text_is_ascii_printable(const char *text) {
    if (!text || !text[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 32 || *p > 126) {
            return false;
        }
    }
    return true;
}

static bool sdl_key_is_plain_printable(const SDL_KeyboardEvent *event) {
    return event && event->type == SDL_KEYDOWN && event->keysym.sym >= 32 && event->keysym.sym <= 126 &&
           (event->keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == 0;
}

static void native_log_sdl_key_event(const SDL_KeyboardEvent *event, bool mapped, bool sent, uint8_t scancode, bool extended) {
    static unsigned log_count = 0;
    if (log_count < 32) {
        fprintf(stderr,
                "[native-input] key %s state=%u repeat=%u sdl_scancode=%d sym=%d mod=0x%x mapped=%d rdp=0x%02x ext=%d sent=%d\n",
                event->type == SDL_KEYDOWN ? "down" : "up", (unsigned)event->state, (unsigned)event->repeat,
                (int)event->keysym.scancode, (int)event->keysym.sym, (unsigned)event->keysym.mod, mapped ? 1 : 0,
                (unsigned)scancode, extended ? 1 : 0, sent ? 1 : 0);
    } else if (log_count == 32) {
        fprintf(stderr, "[native-input] further key event logs suppressed\n");
    }
    log_count++;
}

static void native_log_sdl_text_event(const char *text, bool ascii_printable, bool suppressed, bool sent) {
    static unsigned log_count = 0;
    if (log_count < 16) {
        fprintf(stderr, "[native-input] text bytes=%zu ascii=%d suppressed=%d sent=%d\n", text ? strlen(text) : 0u,
                ascii_printable ? 1 : 0, suppressed ? 1 : 0, sent ? 1 : 0);
    } else if (log_count == 16) {
        fprintf(stderr, "[native-input] further text event logs suppressed\n");
    }
    log_count++;
}

static void native_present_renderer_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (!renderer || !app || app->video_plane_punched) {
        return;
    }
    /* Present the transparent hole-punch frame exactly once. Re-clearing and
     * re-presenting every loop tick raced the ss4s hardware video plane's own buffer
     * swaps on this webOS compositor and produced visible flicker between the (empty)
     * graphics layer and the video plane underneath. */
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    app->video_plane_punched = true;
    if (logged && !*logged) {
        fprintf(stderr, "[native] presented initial SDL renderer frame\n");
        *logged = true;
    }
}

static void native_present_surface_frame(SDL_Window *window, bool *logged) {
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) {
        fprintf(stderr, "[native] SDL_GetWindowSurface failed: %s\n", SDL_GetError());
        return;
    }
    if (SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, 0, 0, 0)) != 0) {
        fprintf(stderr, "[native] SDL_FillRect launch frame failed: %s\n", SDL_GetError());
        return;
    }
    if (SDL_UpdateWindowSurface(window) != 0) {
        fprintf(stderr, "[native] SDL_UpdateWindowSurface launch frame failed: %s\n", SDL_GetError());
        return;
    }
    if (logged && !*logged) {
        fprintf(stderr, "[native] presented initial SDL surface frame\n");
        *logged = true;
    }
}

static int native_present_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (!app || !renderer) {
        return 0;
    }
    int status = 0;
    NativeRgbaResult failed_result = NATIVE_RGBA_OK;
    pthread_mutex_lock(&app->video_lock);
    if (app->pending_texture_destroy) {
        SDL_DestroyTexture(app->pending_texture_destroy);
        app->pending_texture_destroy = NULL;
    }
    if (app->rgba && native_rgba_surface_has_frame(app->rgba)) {
        uint16_t viewport_width = (uint16_t)atomic_load(&app->render_width);
        uint16_t viewport_height = (uint16_t)atomic_load(&app->render_height);
        NativeRgbaResult result = native_rgba_surface_present(app->rgba, renderer, viewport_width, viewport_height);
        if (result == NATIVE_RGBA_OK) {
            status = 1;
            /* RGBA presents opaque content into the same renderer the ss4s hole-punch
             * uses. If the stream later switches back to H.264, the punch-through latch
             * must fire again so the SDL layer clears back to transparent instead of
             * leaving this opaque frame in front of the video plane. */
            app->video_plane_punched = false;
        } else {
            failed_result = result;
            status = -1;
        }
    }
    pthread_mutex_unlock(&app->video_lock);
    if (status > 0 && logged && !*logged) {
        fprintf(stderr, "[native] presented initial native RemoteFX RGBA frame\n");
        *logged = true;
    } else if (status < 0) {
        app->decoder_errors++;
        fprintf(stderr, "[native] terminal native error: DecoderError; RemoteFX RGBA present failed result=%d\n",
                (int)failed_result);
        app_stop_with_state(app, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
    return status;
}

static void native_present_streaming_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (native_present_rgba_frame(app, renderer, logged) == 0) {
        native_present_renderer_frame(app, renderer, logged);
    }
}

static void handle_sdl_event(App *app, SDL_Window *window, SDL_Renderer *renderer, const SDL_Event *event) {
    switch (event->type) {
    case SDL_QUIT:
        fprintf(stderr, "[native] SDL_QUIT requests shutdown\n");
        atomic_store(&app->running, false);
        break;
    case SDL_APP_TERMINATING:
        fprintf(stderr, "[native] SDL_APP_TERMINATING requests shutdown\n");
        atomic_store(&app->running, false);
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        fprintf(stderr, "[native] SDL_APP_WILLENTERBACKGROUND; keeping native session running\n");
        break;
    case SDL_APP_DIDENTERBACKGROUND:
        fprintf(stderr, "[native] SDL_APP_DIDENTERBACKGROUND; keeping native session running\n");
        break;
    case SDL_APP_WILLENTERFOREGROUND:
        fprintf(stderr, "[native] SDL_APP_WILLENTERFOREGROUND\n");
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        fprintf(stderr, "[native] SDL_APP_DIDENTERFOREGROUND\n");
        break;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
            fprintf(stderr, "[native] SDL window close requests shutdown\n");
            atomic_store(&app->running, false);
        } else if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event->window.event == SDL_WINDOWEVENT_RESIZED) {
            fprintf(stderr, "[native] SDL window size event %u: %dx%d\n", (unsigned)event->window.event, event->window.data1,
                    event->window.data2);
            (void)native_update_render_size(app, renderer);
            native_update_pointer_window_size(app);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                   event->window.event == SDL_WINDOWEVENT_SHOWN || event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                   event->window.event == SDL_WINDOWEVENT_EXPOSED) {
            fprintf(stderr, "[native] SDL window lifecycle event %u\n", (unsigned)event->window.event);
        }
        break;
    case SDL_MOUSEMOTION:
        if (app->relative_mouse_active) {
            native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x) + event->motion.xrel,
                                              atomic_load(&app->virtual_mouse_y) + event->motion.yrel);
            native_input_pointer_move(&app->input, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
        } else {
            native_set_virtual_mouse_position(app, event->motion.x, event->motion.y);
            native_input_pointer_move(&app->input, event->motion.x, event->motion.y);
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        NativeInputButton button = native_button_from_sdl(event->button.button);
        if (button != 0) {
            if (!app->relative_mouse_active) {
                native_set_virtual_mouse_position(app, event->button.x, event->button.y);
            }
            native_input_pointer_button(&app->input, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y),
                                        button, event->type == SDL_MOUSEBUTTONDOWN);
        }
        break;
    }
    case SDL_MOUSEWHEEL: {
        int wheel_y = event->wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 4)
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            wheel_y = -wheel_y;
        }
#endif
        native_send_scaled_wheel(app, wheel_y);
        break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (native_handle_webos_red_key(app, &event->key)) {
            break;
        }
        native_hide_webos_cursor_for_key(window, &event->key);
        uint8_t rdp_scancode = 0;
        bool extended = false;
        bool mapped = native_input_sdl_scancode_to_rdp((uint32_t)event->key.keysym.scancode, &rdp_scancode, &extended);
        bool sent = false;
        if (mapped) {
            sent = native_input_key(&app->input, rdp_scancode, event->type == SDL_KEYDOWN, extended);
            if (sent && sdl_key_is_plain_printable(&event->key)) {
                app->suppress_next_ascii_text = true;
                app->suppress_ascii_text_until = SDL_GetTicks() + 250u;
            }
        }
        native_log_sdl_key_event(&event->key, mapped, sent, rdp_scancode, extended);
        break;
    }
    case SDL_TEXTINPUT: {
        bool ascii_printable = sdl_text_is_ascii_printable(event->text.text);
        bool suppressed = false;
        if (ascii_printable && app->suppress_next_ascii_text && SDL_TICKS_PASSED(SDL_GetTicks(), app->suppress_ascii_text_until)) {
            app->suppress_next_ascii_text = false;
        }
        if (ascii_printable && app->suppress_next_ascii_text) {
            app->suppress_next_ascii_text = false;
            suppressed = true;
        }
        bool sent = false;
        if (!suppressed) {
            sent = native_input_text_utf8(&app->input, event->text.text);
        }
        native_log_sdl_text_event(event->text.text, ascii_printable, suppressed, sent);
        break;
    }
    default:
        break;
    }

    (void)window;
}

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
static void native_pump_preconnect_system_events(App *app, SDL_Window *window, SDL_Renderer *renderer, NativePreconnectUi *ui,
                                                 int *window_width, int *window_height) {
    SDL_Event event;
    SDL_PumpEvents();

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) {
        handle_sdl_event(app, window, renderer, &event);
    }

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_APP_TERMINATING, SDL_APP_DIDENTERFOREGROUND) > 0) {
        handle_sdl_event(app, window, renderer, &event);
    }

    SDL_FilterEvents(native_filter_webos_red_key, app);

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT) > 0) {
        handle_sdl_event(app, window, renderer, &event);
        if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESIZED) {
            *window_width = event.window.data1;
            *window_height = event.window.data2;
            native_preconnect_ui_resize(ui, event.window.data1, event.window.data2);
        }
    }

    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) > 0) {
    }
}
#endif

static int native_run_app_loop(App *app, NativeConfig *config, const RdpCallbacks *callbacks) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[native] SDL_InitSubSystem(VIDEO|EVENTS) failed: %s\n", SDL_GetError());
        return 4;
    }
    native_log_sdl_display_modes();

    fprintf(stderr, "[native] creating borderless SDL window %dx%d\n", NATIVE_LOCAL_SURFACE_WIDTH,
            NATIVE_LOCAL_SURFACE_HEIGHT);
    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS;
    SDL_Window *window = SDL_CreateWindow("gnomecast", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          NATIVE_LOCAL_SURFACE_WIDTH, NATIVE_LOCAL_SURFACE_HEIGHT, window_flags);
    if (!window) {
        fprintf(stderr, "[native] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
        fprintf(stderr, "[native] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
#else
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "[native] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
#endif

    int window_width = 0;
    int window_height = 0;
    char surface_message[128];
    if (!native_init_render_state(app, window, renderer, &window_width, &window_height, surface_message,
                                  sizeof(surface_message))) {
        fprintf(stderr, "[native] %s\n", surface_message);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
    SDL_RaiseWindow(window);
    SDL_StartTextInput();

    const uint32_t delay_ms = config->fps > 0 ? (uint32_t)(1000u / config->fps) : 16u;
    fprintf(stderr, "[native] SDL loop running at target %u fps\n", (unsigned)config->fps);

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    native_set_streaming_mouse_mode(app, false);
    NativePreconnectUi *ui = native_preconnect_ui_create(window, renderer, config->host, config->port, config->username,
                                                        config->password, config->domain, config->fps,
                                                        config->relative_mouse);
    if (!ui) {
        fprintf(stderr, "[native-ui] failed to create pre-connect UI\n");
        SDL_StopTextInput();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }

    app->interactive_ui = true;
    int last_ui_state = -1;
    bool present_logged = false;
    bool streaming_visible = false;

    while (atomic_load(&app->running)) {
        int event_state = atomic_load(&app->current_state);
        if (app->rdp && event_state == (int)RDP_STATE_ACTIVE) {
            native_drain_pointer_clamp(app);
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handle_sdl_event(app, window, renderer, &event);
            }
        } else {
            native_pump_preconnect_system_events(app, window, renderer, ui, &window_width, &window_height);
        }

        if (atomic_load(&app->session_failed)) {
            RdpState terminal_state = (RdpState)atomic_load(&app->terminal_state);
            char status[128];
            if (terminal_state == RDP_STATE_STOPPED) {
                (void)snprintf(status, sizeof(status), "Session stopped.");
            } else {
                (void)snprintf(status, sizeof(status), "Connection failed: %s", rdp_state_name(terminal_state));
            }
            native_stop_rdp(app);
            atomic_store(&app->session_failed, false);
            native_set_streaming_mouse_mode(app, false);
            streaming_visible = false;
            last_ui_state = -1;
            native_preconnect_ui_set_visible(ui, true);
            native_preconnect_ui_set_connecting(ui, false, status);
            native_preconnect_ui_set_status(ui, status, true);
        }

        int state = atomic_load(&app->current_state);
        if (app->rdp && state != last_ui_state && state != (int)RDP_STATE_ACTIVE) {
            char status[64];
            (void)snprintf(status, sizeof(status), "%s...", rdp_state_name((RdpState)state));
            native_preconnect_ui_set_status(ui, status, false);
            last_ui_state = state;
        }
        if (app->rdp && state == (int)RDP_STATE_ACTIVE && !streaming_visible) {
            (void)native_config_save_persisted(config);
            native_preconnect_ui_set_visible(ui, false);
            native_set_streaming_mouse_mode(app, true);
            streaming_visible = true;
        }

        if (streaming_visible) {
            native_present_streaming_frame(app, renderer, &present_logged);
        }

        char requested_host[NATIVE_CONFIG_STRING_MAX];
        char requested_username[NATIVE_CONFIG_STRING_MAX];
        char requested_password[NATIVE_CONFIG_STRING_MAX];
        char requested_domain[NATIVE_CONFIG_STRING_MAX];
        uint16_t requested_port = 0;
        uint16_t requested_fps = 0;
        bool requested_relative_mouse = false;
        if (!app->rdp &&
            native_preconnect_ui_take_connect(ui, requested_host, sizeof(requested_host), &requested_port,
                                             requested_username, sizeof(requested_username), requested_password,
                                             sizeof(requested_password), requested_domain, sizeof(requested_domain),
                                             &requested_fps, &requested_relative_mouse)) {
            if (!copy_config_string(config->host, sizeof(config->host), requested_host, "host")) {
                native_preconnect_ui_set_status(ui, "Host value is too long.", true);
            } else if (!copy_config_string(config->username, sizeof(config->username), requested_username, "username")) {
                native_preconnect_ui_set_status(ui, "Username value is too long.", true);
            } else if (!copy_config_string(config->password, sizeof(config->password), requested_password, "password")) {
                native_preconnect_ui_set_status(ui, "Password value is too long.", true);
            } else if (!copy_config_string(config->domain, sizeof(config->domain), requested_domain, "domain")) {
                native_preconnect_ui_set_status(ui, "Domain value is too long.", true);
            } else {
                config->port = requested_port;
                config->fps = requested_fps;
                native_config_apply_initial_desktop_hint(config);
                config->relative_mouse = requested_relative_mouse;
                char validation_message[128];
                if (!native_config_validate_connect(config, validation_message, sizeof(validation_message))) {
                    native_preconnect_ui_set_status(ui, validation_message, true);
                } else {
                    native_config_log_effective(config);
                    native_preconnect_ui_set_connecting(ui, true, "Connecting...");
                    if (!native_start_rdp(app, config, callbacks)) {
                        native_preconnect_ui_set_connecting(ui, false, "Connection failed: start failed");
                        native_preconnect_ui_set_status(ui, "Connection failed: start failed", true);
                    } else {
                        native_update_pointer_window_size(app);
                    }
                }
            }
        }

        native_preconnect_ui_tick(ui);
        SDL_Delay(delay_ms == 0 ? 1 : delay_ms);
    }

    native_preconnect_ui_destroy(ui);
    native_set_streaming_mouse_mode(app, false);
#else
    bool present_logged = false;
    if (!native_start_rdp(app, config, callbacks)) {
        SDL_StopTextInput();
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        int exit_code = atomic_load(&app->exit_code);
        return exit_code ? exit_code : 2;
    }
    native_update_pointer_window_size(app);
    native_set_streaming_mouse_mode(app, true);

    while (atomic_load(&app->running)) {
        native_drain_pointer_clamp(app);
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle_sdl_event(app, window, renderer, &event);
        }

        if (renderer) {
            native_present_streaming_frame(app, renderer, &present_logged);
        } else {
            native_present_surface_frame(window, &present_logged);
        }
        SDL_Delay(delay_ms == 0 ? 1 : delay_ms);
    }
    native_set_streaming_mouse_mode(app, false);
#endif

    SDL_StopTextInput();
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    return atomic_load(&app->exit_code);
}
#else
static int native_run_app_loop(App *app, NativeConfig *config, const RdpCallbacks *callbacks) {
    (void)config;
    (void)callbacks;
    fprintf(stderr,
            "[native] SDL event loop is not compiled in; deterministic smoke loop exits after start "
            "(enable HELLOLG_WITH_SDL for webOS lifecycle/input)\n");
    return atomic_load(&app->exit_code);
}
#endif

int main(int argc, char **argv) {
    native_prepare_webos_logging();
    native_prepare_webos_environment();

    NativeConfig native_config;
    native_config_defaults(&native_config);

    const char *config_path = arg_value(argc, argv, "--config", NATIVE_CONFIG_PATH);
    bool config_required = arg_exists(argc, argv, "--config");
    if (!native_config_load_file(&native_config, config_path, config_required) ||
        !native_config_apply_launch_params(&native_config, argc, argv) || !native_config_apply_cli(&native_config, argc, argv)) {
        return 2;
    }
    native_config_apply_initial_desktop_hint(&native_config);

    int sdl_result = native_prepare_sdl_runtime();
    if (sdl_result != 0) {
        return sdl_result;
    }

    bool ignore_saved_config = native_config_launch_ignores_saved_config(argc, argv);
    if (!native_config_load_persisted(&native_config, ignore_saved_config) ||
        (config_required && !native_config_load_file(&native_config, config_path, config_required)) ||
        !native_config_apply_launch_params(&native_config, argc, argv) || !native_config_apply_cli(&native_config, argc, argv)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
    native_config_apply_initial_desktop_hint(&native_config);

    native_config_log_effective(&native_config);
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    if (!native_config_validate_runtime(&native_config)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
#else
    if (!native_config_validate(&native_config)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
#endif

    App app;
    memset(&app, 0, sizeof(app));
    if (pthread_mutex_init(&app.video_lock, NULL) != 0) {
        native_shutdown_sdl_runtime();
        return 2;
    }
    app.desktop_width = native_config.width;
    app.desktop_height = native_config.height;
    app.target_fps = native_config.fps;
    app.password_for_redaction = native_config.password;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    app.wheel_step = native_config.wheel_step;
    app.wheel_scroll_divisor = native_config.wheel_scroll_divisor;
    atomic_init(&app.render_width, 0);
    atomic_init(&app.render_height, 0);
#endif
    atomic_init(&app.running, true);
    atomic_init(&app.session_failed, false);
    atomic_init(&app.exit_code, 0);
    atomic_init(&app.current_state, (int)RDP_STATE_IDLE);
    atomic_init(&app.terminal_state, (int)RDP_STATE_IDLE);
    native_input_init(&app.input, NULL, native_config.width, native_config.height);

    RdpCallbacks callbacks = {
        .ctx = &app,
        .on_state = on_state,
        .on_log = on_log,
        .on_desktop_size = on_desktop_size,
        .on_bitmap_update = on_bitmap_update,
        .on_video_au = on_video_au,
        .on_audio_format = on_audio_format,
        .on_audio_data = on_audio_data,
    };

#if !defined(HELLOLG_WITH_SDL) || !HELLOLG_WITH_SDL
    if (!native_start_rdp(&app, &native_config, &callbacks)) {
        int exit_code = atomic_load(&app.exit_code);
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return exit_code ? exit_code : 2;
    }
#endif

    int loop_result = native_run_app_loop(&app, &native_config, &callbacks);
    if (loop_result != 0 && atomic_load(&app.exit_code) == 0) {
        atomic_store(&app.exit_code, loop_result);
    }

    native_stop_rdp(&app);
    pthread_mutex_destroy(&app.video_lock);
    native_shutdown_sdl_runtime();

    int exit_code = atomic_load(&app.exit_code);
    if (exit_code == 0 && app.decoder_errors > 0) {
        exit_code = rdp_state_exit_code(RDP_STATE_DECODER_ERROR);
    }
    return exit_code;
}
