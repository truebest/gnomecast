#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

#include "config_paths.h"
#include "cursor_sdl.h"
#if (defined(HELLOLG_WITH_EVDEV_MOUSE) && HELLOLG_WITH_EVDEV_MOUSE) || \
    (defined(HELLOLG_WITH_EVDEV_KEYBOARD) && HELLOLG_WITH_EVDEV_KEYBOARD)
#define HELLOLG_WITH_EVDEV_INPUT 1
#include "input_evdev.h"
#else
#define HELLOLG_WITH_EVDEV_INPUT 0
#endif
#include "input_sdl.h"
#include "rdp_ffi.h"
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
#include "ui_preconnect.h"
#else
typedef void NativePreconnectUi;
#endif
#include "audio_mixer.h"
#include "audio_opus.h"
#include "ui_mixer.h"
#include "audio_ss4s.h"
#include "media_ss4s.h"
#include "settings_json.h"
#include "video_rgba_sdl.h"
#include "video_ss4s.h"

#define NATIVE_RDP_INITIAL_DESKTOP_WIDTH 1920u
#define NATIVE_RDP_INITIAL_DESKTOP_HEIGHT 1080u
#define NATIVE_APP_ID "com.truebest.gnomecast.native"
/* webOS always renders the app's graphics/UI plane on a virtual ~1920x1080 logical canvas
 * that the platform scales to the panel; the video decoder plane is independent and can run
 * at the panel's true native resolution regardless of this. There is no benefit to a locally
 * larger SDL surface, so this is fixed rather than user-selectable. */
#define NATIVE_LOCAL_SURFACE_WIDTH 1920
#define NATIVE_LOCAL_SURFACE_HEIGHT 1080

#define NATIVE_CONFIG_PATH "native/config.local.json"
#define NATIVE_CONFIG_MAX_FILE (16u * 1024u)
#define NATIVE_CONFIG_STRING_MAX NATIVE_SETTINGS_STRING_MAX
/* NATIVE_PERSISTED_CONFIG_{FILENAME,PATH_MAX,MAX_CANDIDATES} live in config_paths.h. */

/* PCM chunk the mixer produces per tick: ~21ms at 48kHz, commensurate with
 * gnome-remote-desktop's packetization (Opus 20ms/960 frames; legacy PCM 1024-frame
 * waves). 480 frames (10ms) was tried live and works — PLAYING intact, ~11ms less
 * standing latency — but doubles the pump cadence for a gain below hearing, so the
 * conservative quantum stays. */
#define NATIVE_MIXER_CHUNK_FRAMES 1024u
/* Per-source ring bound: 65536 frames (~1.49s at 44.1kHz, 256KB). The capacity IS the
 * audio latency cap — drop-oldest at the boundary bounds how far behind a source can
 * fall, with no separate trimming heuristics. Sized for two of the ~700ms waves old
 * gnome-remote-desktop versions ship; on modern servers (23ms packets) the ring idles
 * near the prebuffer. */
#define NATIVE_MIXER_CAPACITY_FRAMES 65536u
/* The expected mix format: the client advertises Opus 48k + PCM, and gnome-remote-desktop
 * prefers Opus among the client's formats, so sessions land on 48kHz stereo (Opus decodes
 * at its native 48k). A PCM-only server at another rate re-pins the mixer on the first
 * negotiated format (see on_audio_format). */
#define NATIVE_AUDIO_MIX_DEFAULT_RATE 48000u
#define NATIVE_AUDIO_MIX_DEFAULT_CHANNELS 2u
/* If the freshly switched-to session produces no decodable frame within this window,
 * force a reconnect (guaranteed IDR) — covers servers where neither suppress-resume nor
 * the display-control refresh yields a keyframe (e.g. grd mirror mode). */
#define NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS 2000u
/* How long the colored active-session indicator stays on screen after a switch. */
#define NATIVE_INDICATOR_SHOW_MS 1500u
/* Volume-mixer overlay: the fader model constants (range, step, auto-hide) and both
 * renderers live in ui_mixer.h/.c; this file keeps the state machine and key routing. */

_Static_assert(NATIVE_MIXER_MAX_SOURCES >= NATIVE_SETTINGS_MAX_SESSIONS,
               "every session slot needs a mixer source");

typedef struct App App;

/* One remote-button session slot (green/yellow). The slot owns its RDP session handle,
 * connection config (stable while connected — on_log redaction and reconnects read it),
 * per-session server state (desktop size, cursor) and its audio-mixer routing. The slot
 * struct itself is the callback ctx for all RdpCallbacks of its session. */
typedef struct NativeSessionSlot {
    App *app;
    int index; /* NATIVE_SESSION_SLOT_* */
    RdpSession *rdp;
    NativeSessionConfig config;
    atomic_int current_state;
    atomic_int terminal_state;
    /* Set by worker callbacks on terminal events; drained on the SDL thread, which
     * decides between auto-switching to the surviving session and showing the UI. */
    atomic_bool session_failed;
    /* Server EGFX graphics output size (worker writes, SDL thread reads on switch). */
    atomic_uint desktop_width;
    atomic_uint desktop_height;
    /* Bumped by THIS slot's worker whenever one of its frames decodes OK (H.264 or
     * RemoteFX RGBA). The switch watchdog baselines the TARGET slot's counter: with a
     * single global counter, a frame the previous owner had in flight during the switch
     * could bump it after the baseline was armed and satisfy the watchdog without the
     * new slot ever decoding anything. */
    atomic_uint video_ok_frames;
    /* Server-driven cursor. Background sessions keep caching their latest shape here;
     * the SDL thread applies only the active slot's cursor each tick. */
    NativeCursor cursor;
    /* Audio-mixer routing (worker thread of this session only). */
    bool audio_routed;
    bool audio_incompatible_logged;
    /* Per-session Opus decoder (worker thread; created in on_audio_format, destroyed
     * after the worker is joined in native_stop_slot). NULL for raw-PCM sessions. */
    NativeOpusDecoder *opus_decoder;
    /* SDL thread only: whether we asked the server to suppress graphics (background). */
    bool suppressed;
    /* Bumped on every (re)connect of this slot; with the slot index it identifies which
     * stream generation the shared video decoder holds (see App.video_owner_*). */
    atomic_uint connect_epoch;
    /* Diagnostics: AUs of this stream dropped while waiting for its keyframe after a
     * switch (worker thread; logged throttled). */
    atomic_uint keyframe_wait_drops;
    /* SDL thread only: a switch to this slot already had to fall back to the watchdog
     * reconnect — its server yields no keyframe on request (no Display Control channel,
     * Refresh Rect ignored). Subsequent switches reconnect immediately instead of
     * burning the watchdog timeout on a keyframe that will not come. */
    bool refresh_ineffective;
} NativeSessionSlot;

typedef struct App {
    NativeSessionSlot sessions[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Which slot owns the screen: it feeds the single hardware video plane (when
     * connected) or shows its configurator form (when not), and receives input. Written
     * on the SDL thread, read by every worker callback for routing. */
    atomic_int active_index;
    /* SDL thread only: keyframe deadline after a video switch (0 = no deadline).
     * switch_baseline_frames snapshots the WATCHED slot's video_ok_frames. */
    uint32_t switch_deadline_ticks;
    unsigned switch_baseline_frames;
    bool switch_reconnect_used;
    /* Set by worker callbacks that had to drop the live video track (audio-track open
     * reloads the shared pipeline); the SDL thread turns it into rdp_request_refresh +
     * the keyframe watchdog — gnome-remote-desktop never resends an IDR on its own. */
    atomic_bool video_refresh_needed;
    NativeRgbaSurface *rgba;
    /* Shared SS4S library/player owner; video and audio attach tracks to it. Guarded by
     * video_lock like the tracks themselves. */
    NativeMedia *media;
    NativeVideo *video;
    /* Which slot/connection generation the open video track decodes (video_lock). A
     * mismatch against the arriving AU's slot+epoch means the decoder holds a FOREIGN
     * stream's state: the swap to the new stream is deferred until its keyframe is in
     * hand, so the old picture stays up (no black gap) and the shared pipeline reloads
     * exactly once per switch (one audio interruption instead of two). */
    int video_owner_slot;
    unsigned video_owner_epoch;
    /* The single mixed-PCM audio track; all sessions' audio flows through the mixer into
     * it. Format == mixer format (video_lock). */
    NativeAudio *audio;
    /* Sums per-session PCM rings; its pump thread feeds app->audio. The mixer object is
     * internally locked; init/start state below is guarded by video_lock. */
    NativeAudioMixer mixer;
    bool mixer_started;
    uint32_t mixer_sample_rate;
    uint16_t mixer_channels;
    uint16_t audio_prebuffer_ms; /* per-source mixer jitter prebuffer, from settings */
    uint16_t audio_codec;        /* NATIVE_AUDIO_CODEC_*: auto (Opus) or lossless PCM */
    /* Per-slot fader position in dB (NATIVE_MIXER_FADER_MIN_DB..MAX_DB, 3 dB steps,
     * default 0 = unity; the bottom stop mutes). Only the SDL thread writes it (volume
     * overlay), under video_lock: workers re-read the set in native_ensure_mixer_locked
     * when a codec change re-pins the mixer, so the levels survive re-inits. */
    int8_t mixer_gain_db[NATIVE_SETTINGS_MAX_SESSIONS];
    pthread_mutex_t video_lock;
    /* Guards slot config strings between the SDL thread overwriting a slot's config on
     * (re)Connect and OTHER slots' rdp-workers scanning every config in
     * redact_if_sensitive. A slot's own worker is joined before its config is written,
     * but that never protected the cross-slot scan. */
    pthread_mutex_t redaction_lock;
    /* Serializes the mixer re-pin (pump stop -> destroy -> reinit) across rdp-workers:
     * the mixer's internal control_lock cannot protect its own destruction. Taken
     * BEFORE video_lock (see on_audio_format); never taken by the pump thread. */
    pthread_mutex_t mixer_repin_lock;
    NativeInput input;
#if HELLOLG_WITH_EVDEV_INPUT
    /* Raw evdev mouse+keyboard reader (active during streaming); one background thread polls
     * grabbed /dev/input devices and wakes the SDL loop through eventfd. */
    NativeEvdevInput evdev_input;
#endif
    int decoder_errors;
    bool decoder_keyframe_pending;
    /* Set once the SDL graphics layer has presented a single transparent frame so the
     * ss4s hardware video plane underneath shows through. Re-presenting a transparent
     * frame every loop tick raced the video plane's own buffer swaps and produced
     * visible flicker, so this latches to "present once, then leave the window alone". */
    bool video_plane_punched;
    atomic_bool running;
    atomic_int exit_code;
    bool interactive_ui;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    /* Colored square shown briefly after a session switch (SDL thread only). */
    int indicator_slot;
    uint32_t indicator_until_ticks;
    bool indicator_drawn;
    /* Volume-mixer overlay over the live stream (SDL thread only): opened by re-pressing
     * the ACTIVE slot's color button; one vertical slider per slot adjusts that mixer
     * source live. Latch-drawn like the indicator (mixer_overlay_drawn). */
    bool mixer_overlay_visible;
    bool mixer_overlay_drawn;
    int mixer_overlay_selected;
    uint32_t mixer_overlay_hide_ticks;
    /* Left button held on a fader: motion drags the selected knob (SDL thread only). */
    bool mixer_overlay_dragging;
    /* SDL thread only: an opaque switch splash currently covers the video plane (drawn
     * while the keyframe watchdog is armed, i.e. a swap is in flight; the pipeline
     * reload window would otherwise show through as a black screen). */
    bool switch_splash_drawn;
    /* SDL thread only: latched when the streaming screen's side effects ran (UI hidden,
     * evdev grabbed); cleared when the screen returns to a configurator. */
    bool streaming_visible;
    /* SDL thread only: NumLock was synced to the current active session. Input arming is
     * derived on the SDL thread each tick (worker callbacks must not write shared input
     * state — a demoted slot's stale write could race a switch and strand input off). */
    bool input_locks_synced;
    /* SDL thread only: last connection state mirrored into the configurator status. */
    int ui_last_state;
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    /* Borrowed while native_run_app_loop is on the preconnect screen, so the SDL event
     * filter can route green/yellow remote keys to the slot selector. */
    NativePreconnectUi *preconnect_ui;
#endif
    uint16_t wheel_step;
    uint16_t wheel_scroll_divisor;
    int wheel_accumulator;
    /* Only ever written by the SDL thread (the evdev mouse drain and the pointer-clamp/warp
     * drains below), so plain last-writer-wins races with a second writer can't happen;
     * atomic only for torn-access safety against the RDP worker thread's reads via
     * native_send_scaled_wheel()/native_update_pointer_window_size(). */
    atomic_int virtual_mouse_x;
    atomic_int virtual_mouse_y;
    /* Set by the RDP worker thread (on_desktop_size, on a RDPGFX_RESET_GRAPHICS_PDU) to ask
     * the SDL thread to re-clamp virtual_mouse_x/y into the new window bounds on its next
     * loop tick, keeping the SDL thread the sole writer of virtual_mouse_x/y. */
    atomic_bool pointer_clamp_pending;
    /* Server-initiated pointer warp (on_pointer_position, RDP worker thread) in desktop
     * coordinates; the SDL thread repositions virtual_mouse_x/y and warps the real mouse on
     * its next tick so it stays the sole writer of virtual_mouse_x/y. */
    atomic_bool pointer_warp_pending;
    atomic_uint pointer_warp_x;
    atomic_uint pointer_warp_y;
    atomic_uint render_width;
    atomic_uint render_height;
    /* SDL thread only. Set true on window FOCUS_LOST (a remote-invoked webOS overlay such as
     * the TV menu steals focus without backgrounding the app); while set, the evdev grabs are
     * released and the SDL pointer fallback is ignored so input drives the overlay, not RDP.
     * Zero-initialised, i.e. focused. */
    bool window_unfocused;
    /* SDL thread only. Set on focus regain: a webOS overlay leaves the platform cursor hidden,
     * and because we grab the mouse the compositor never sees the pointer activity that would
     * auto-re-show it — and a bare visibility call at focus-gain (no pointer movement, e.g. the
     * menu was closed with Esc) does not always take. So we also re-assert the cursor on the
     * first real mouse movement after regaining focus, when the visibility call coincides with
     * genuine activity. One-shot. */
    bool cursor_reassert_pending;
    /* SDL thread only. Which inputs we have forwarded a down for but not yet an up, so their
     * releases can be flushed to the server before the evdev grab is dropped on focus loss —
     * otherwise the up goes to the overlay, not RDP, leaving a stuck drag or auto-repeating
     * key. held_mouse is indexed by NativeInputButton-1 (LEFT/RIGHT/MIDDLE); held_keys is a
     * bitset over scancode | (extended ? 0x100 : 0). */
    bool held_mouse[3];
    uint8_t held_keys[64];
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

static NativeSessionSlot *native_active_slot(App *app) {
    return &app->sessions[atomic_load(&app->active_index)];
}

static bool native_slot_is_active(const NativeSessionSlot *slot) {
    return atomic_load(&slot->app->active_index) == slot->index;
}

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
    /* The EXIT key stays with the system (no ACCESS_POLICY_KEYS_EXIT hint): webOS closes
     * the app itself and we shut down on the resulting SDL_QUIT/SDL_APP_TERMINATING. */
    /* LSM's pointer auto-hide (mrcu standby) timer. moonlight-tv uses 5000ms because the
     * game host draws its own cursor; for a remote desktop the pointer must not vanish —
     * once LSM detaches it, motion and button events stop being delivered entirely and
     * only wheel/keyboard input can re-summon it. Effectively disable the auto-hide;
     * hide-while-typing still works via the explicit SDL_webOSCursorVisibility calls. */
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "86400000");
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

static bool native_config_apply_json_if_present(NativeSettings *settings, const char *json, const char *source,
                                                 bool *applied) {
    if (!native_settings_json_has_rdp_key(json)) {
        return true;
    }
    if (!native_settings_apply_json(settings, json, source)) {
        fprintf(stderr, "[native] failed to parse %s\n", source);
        return false;
    }
    fprintf(stderr, "[native] loaded %s\n", source);
    *applied = true;
    return true;
}

/* Reads the whole file at `path` into a NUL-terminated heap buffer/* Reads the whole file at `path` into a NUL-terminated heap buffer with a single open.
 *   missing file (open fails):            *existed=false, *out=NULL, returns true
 *   read/size error on an existing file:  *existed=true,  *out=NULL, returns false
 *   success:                              *existed=true,  *out=buffer (caller frees), returns true */
static bool native_config_read_file(const char *path, char **out, bool *existed) {
    *out = NULL;
    *existed = false;

    FILE *file = fopen(path, "rb");
    if (!file) {
        return true;
    }
    *existed = true;

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

    *out = json;
    return true;
}

static bool native_config_load_file_internal(NativeSettings *settings, const char *path, bool required, bool log_missing) {
    char *json = NULL;
    bool existed = false;
    if (!native_config_read_file(path, &json, &existed)) {
        return false;
    }
    if (!existed) {
        if (required) {
            fprintf(stderr, "[native] config file not found: %s\n", path);
            return false;
        }
        if (log_missing) {
            fprintf(stderr, "[native] config file not found at %s; using defaults and CLI overrides\n", path);
        }
        return true;
    }

    bool ok = native_settings_apply_json(settings, json, path);
    free(json);
    if (!ok) {
        fprintf(stderr, "[native] failed to parse config file: %s\n", path);
        return false;
    }
    fprintf(stderr, "[native] loaded config file: %s\n", path);
    return true;
}

static bool native_config_load_file(NativeSettings *settings, const char *path, bool required) {
    return native_config_load_file_internal(settings, path, required, true);
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static void native_config_collect_persisted_candidates(NativeConfigPathCandidates *candidates) {
    memset(candidates, 0, sizeof(*candidates));

    /* User-provided overrides are trusted (from_env=true): they are honored for reads in
     * priority order even on a read-only or foreign-owned mount. Discovered locations below
     * must pass the ownership/permission check before their contents are written or trusted. */
    (void)native_config_add_candidate_path(candidates, getenv("HELLOLG_NATIVE_SETTINGS_PATH"), true);
    (void)native_config_add_candidate_dir(candidates, getenv("HELLOLG_NATIVE_SETTINGS_DIR"), true);

    char *pref_path = SDL_GetPrefPath("truebest", "gnomecast");
    if (pref_path) {
        (void)native_config_add_candidate_dir(candidates, pref_path, false);
        SDL_free(pref_path);
    } else {
        fprintf(stderr, "[native] failed to resolve SDL pref path: %s\n", SDL_GetError());
    }

    const char *appid = getenv("APPID");
    if (!appid || !appid[0]) {
        appid = NATIVE_APP_ID;
    }

    /* Preferred location (user choice): the IPK ships <approot>/settings with mode
     * 01777 (tools/build-native-webos.sh) because the install tree is root-owned on the
     * TV; the app-private euid-suffixed subdir inside follows the exact trust model of
     * the /tmp fallback — but on persistent storage. The dev-mode jail sets HOME to the
     * app root itself (verified live; SDL_GetPrefPath is what points at bin/). */
    const char *home = getenv("HOME");
    if (home && home[0] && strcmp(home, "/") != 0) {
        char inapp_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
        int an = snprintf(inapp_dir, sizeof(inapp_dir), "%s/settings/%lu", home, (unsigned long)geteuid());
        if (an > 0 && (size_t)an < sizeof(inapp_dir)) {
            (void)native_config_add_candidate_dir(candidates, inapp_dir, false);
        }
    }

    (void)native_config_add_candidate_app_dir(candidates, "/var/luna/preferences/%s", appid);
    (void)native_config_add_candidate_app_dir(candidates, "/media/developer/apps/usr/palm/data/%s", appid);
    /* Dev-mode app data commonly lives on cryptofs (persistent ext4). */
    (void)native_config_add_candidate_app_dir(candidates, "/media/cryptofs/apps/usr/palm/data/%s", appid);
    (void)native_config_add_candidate_app_dir(candidates, "/media/internal/%s", appid);

    /* /media/developer is the ONE persistent ext4 the dev-mode jail maps read-write
     * (verified live via /proc/self/mounts — every conventional data dir above is
     * root-owned or a read-only mount for this app's uid). Its temp/ is world-writable
     * like /tmp, so an app-private euid-suffixed subdir passes the exact ownership
     * checks the /tmp fallback below relies on — but it survives TV power cycles, which
     * tmpfs does not (observed live: settings vanished on the first cold boot). */
    char devtemp_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    int dn = snprintf(devtemp_dir, sizeof(devtemp_dir), "/media/developer/temp/%s-%lu", appid,
                      (unsigned long)geteuid());
    if (dn > 0 && (size_t)dn < sizeof(devtemp_dir)) {
        (void)native_config_add_candidate_dir(candidates, devtemp_dir, false);
    }

    (void)native_config_add_candidate_app_dir(candidates, "/var/run/%s", appid);

    char fallback_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    int n = snprintf(fallback_dir, sizeof(fallback_dir), "/tmp/%s-%lu", appid, (unsigned long)geteuid());
    if (n > 0 && (size_t)n < sizeof(fallback_dir)) {
        (void)native_config_add_candidate_dir(candidates, fallback_dir, false);
    }
}

static bool native_config_get_persisted_save_path(char *path, size_t cap) {
    NativeConfigPathCandidates candidates;
    native_config_collect_persisted_candidates(&candidates);

    size_t index = 0;
    if (native_config_find_persisted_save_candidate(&candidates, &index)) {
        if (!native_config_copy_path(path, cap, candidates.paths[index])) {
            fprintf(stderr, "[native] persisted config path is too long\n");
            return false;
        }
        fprintf(stderr, "[native] using persisted config path: %s\n", path);
        return true;
    }

    fprintf(stderr, "[native] no writable persisted config directory found\n");
    return false;
}

typedef enum NativeConfigLoadOutcome {
    NATIVE_CONFIG_LOAD_MISSING, /* no file at this path */
    NATIVE_CONFIG_LOAD_INVALID, /* file exists but could not be read or parsed */
    NATIVE_CONFIG_LOAD_OK,      /* file parsed and applied into *config */
} NativeConfigLoadOutcome;

/* Load one candidate with a single open, distinguishing "no file here" from "file is
 * corrupt" so the caller can fall through to lower-priority candidates in either case. */
static NativeConfigLoadOutcome native_config_try_load_candidate(NativeSettings *settings, const char *path) {
    char *json = NULL;
    bool existed = false;
    if (!native_config_read_file(path, &json, &existed)) {
        return existed ? NATIVE_CONFIG_LOAD_INVALID : NATIVE_CONFIG_LOAD_MISSING;
    }
    if (!existed) {
        return NATIVE_CONFIG_LOAD_MISSING;
    }

    bool ok = native_settings_apply_json(settings, json, path);
    free(json);
    if (!ok) {
        return NATIVE_CONFIG_LOAD_INVALID;
    }
    return NATIVE_CONFIG_LOAD_OK;
}

static bool native_config_load_persisted(NativeSettings *settings, bool force_ignore) {
    const char *ignore = getenv("HELLOLG_IGNORE_SAVED_CONFIG");
    if (force_ignore || (ignore && strcmp(ignore, "1") == 0)) {
        fprintf(stderr, "[native] skipped persisted config because saved settings were disabled for this launch\n");
        return true;
    }

    NativeConfigPathCandidates candidates;
    native_config_collect_persisted_candidates(&candidates);

    /* Load from the highest-priority candidate whose file exists and parses. Writability plays
     * no part in load ordering (an env override on a read-only mount must still win over a
     * stale writable copy), and a corrupt higher-priority file must not hide a valid
     * lower-priority one. Discovered (non-env) directories must be owned by us and not
     * other-writable before their contents are trusted, so a local attacker cannot plant a
     * settings.json (pointing at their own RDP host) for us to load. */
    for (size_t i = 0; i < candidates.count; i++) {
        if (!candidates.from_env[i]) {
            char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
            if (!native_config_parent_dir(candidates.paths[i], dir, sizeof(dir)) ||
                !native_config_dir_is_secure(dir)) {
                continue;
            }
        }

        NativeConfigLoadOutcome outcome = native_config_try_load_candidate(settings, candidates.paths[i]);
        if (outcome == NATIVE_CONFIG_LOAD_MISSING) {
            continue;
        }
        if (outcome == NATIVE_CONFIG_LOAD_INVALID) {
            fprintf(stderr, "[native] ignored invalid persisted config: %s\n", candidates.paths[i]);
            continue;
        }
        fprintf(stderr, "[native] loaded persisted config: %s\n", candidates.paths[i]);
        return true;
    }
    return true;
}

static bool native_config_save_persisted(const NativeSettings *settings) {
    /* The winning save path is normally constant for the process lifetime, so resolve it once
     * (the resolution runs mkdir -p plus a write probe across several directories) and reuse it
     * on every connect instead of re-probing the filesystem each time settings are saved. */
    static char cached_path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    static bool have_cached_path = false;

    if (have_cached_path && native_settings_save_file(settings, cached_path)) {
        return true;
    }

    /* Either nothing is cached yet, or the cached save just failed — most likely because the
     * directory was removed mid-session (e.g. a /tmp reaper). Re-resolve (which re-creates or
     * re-selects a writable, secure directory) and write there in the SAME call, so a save that
     * just captured the user's updated credentials is not lost until the next connect. */
    have_cached_path = false;
    if (!native_config_get_persisted_save_path(cached_path, sizeof(cached_path))) {
        return false;
    }
    have_cached_path = true;
    return native_settings_save_file(settings, cached_path);
}
#else
static bool native_config_load_persisted(NativeSettings *settings, bool force_ignore) {
    (void)settings;
    (void)force_ignore;
    return true;
}

static bool native_config_save_persisted(const NativeSettings *settings) {
    (void)settings;
    return true;
}
#endif

static bool native_config_apply_launch_json_string(NativeSettings *settings, const char *json, const char *key,
                                                   int arg_index, bool *arg_applied) {
    const char *value = native_json_find_value(json, key);
    if (!value || *native_json_skip_ws(value) != '"') {
        return true;
    }

    char nested[NATIVE_CONFIG_MAX_FILE];
    int result = native_json_read_string(json, key, nested, sizeof(nested));
    if (result < 0) {
        fprintf(stderr, "[native] invalid string value for webOS launch field %s\n", key);
        return false;
    }
    if (result == 0) {
        return true;
    }

    const char *nested_json = native_json_skip_ws(nested);
    if (!nested_json || nested_json[0] != '{') {
        return true;
    }
    char source[96];
    (void)snprintf(source, sizeof(source), "webOS launch argument %d %s JSON", arg_index, key);
    return native_config_apply_json_if_present(settings, nested_json, source, arg_applied);
}

static bool native_config_apply_launch_params(NativeSettings *settings, int argc, char **argv) {
    bool saw_launch_json = false;
    bool applied = false;
    for (int i = 1; i < argc; i++) {
        const char *params = native_json_skip_ws(argv[i]);
        if (!params || params[0] != '{') {
            continue;
        }

        saw_launch_json = true;
        bool arg_applied = false;
        char source[64];
        (void)snprintf(source, sizeof(source), "webOS launch argument %d", i);
        if (!native_config_apply_json_if_present(settings, params, source, &arg_applied) ||
            !native_config_apply_launch_json_string(settings, params, "params", i, &arg_applied) ||
            !native_config_apply_launch_json_string(settings, params, "launchParams", i, &arg_applied)) {
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
    if (native_json_read_bool(json, "ignoreSavedConfig", &ignore) > 0 && ignore) {
        return true;
    }

    for (const char **key = (const char *[]){"params", "launchParams", NULL}; *key; key++) {
        char nested[NATIVE_CONFIG_MAX_FILE];
        int result = native_json_read_string(json, *key, nested, sizeof(nested));
        if (result <= 0) {
            continue;
        }
        const char *nested_json = native_json_skip_ws(nested);
        if (native_config_launch_json_ignores_saved_config(nested_json)) {
            return true;
        }
    }
    return false;
}

static bool native_config_launch_ignores_saved_config(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *params = native_json_skip_ws(argv[i]);
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

static bool apply_cli_audio_codec(int argc, char **argv, NativeSettings *settings) {
    const char *value = arg_value(argc, argv, "--audio-codec", NULL);
    if (!value) {
        return true;
    }
    if (strcmp(value, "auto") == 0 || strcmp(value, "opus") == 0) {
        settings->audio_codec = NATIVE_AUDIO_CODEC_AUTO;
        return true;
    }
    if (strcmp(value, "pcm") == 0) {
        settings->audio_codec = NATIVE_AUDIO_CODEC_PCM;
        return true;
    }
    fprintf(stderr, "[native] invalid value for --audio-codec (expected auto or pcm)\n");
    return false;
}

static bool native_config_apply_cli(NativeSettings *settings, int argc, char **argv) {
    /* Flat CLI flags keep their historical meaning and target the GREEN slot; the yellow
     * slot is configured via the settings file, launch JSON ("sessions" array) or UI. */
    NativeSessionConfig *green = &settings->sessions[NATIVE_SESSION_SLOT_GREEN];
    if (!(apply_cli_string(argc, argv, "--host", green->host, sizeof(green->host), "host") &&
          apply_cli_string(argc, argv, "--user", green->username, sizeof(green->username), "username") &&
          apply_cli_string(argc, argv, "--username", green->username, sizeof(green->username), "username") &&
          apply_cli_string(argc, argv, "--password", green->password, sizeof(green->password), "password") &&
          apply_cli_string(argc, argv, "--domain", green->domain, sizeof(green->domain), "domain") &&
          apply_cli_u16(argc, argv, "--port", 1, UINT16_MAX, &green->port) &&
          apply_cli_u16(argc, argv, "--width", 1, UINT16_MAX, &settings->width) &&
          apply_cli_u16(argc, argv, "--height", 1, UINT16_MAX, &settings->height) &&
          apply_cli_u16(argc, argv, "--fps", 1, 240, &green->fps) &&
          apply_cli_u16(argc, argv, "--wheel-step", 1, 120, &settings->wheel_step) &&
          apply_cli_u16(argc, argv, "--wheel-scroll-divisor", 1, 120, &settings->wheel_scroll_divisor) &&
          apply_cli_u16(argc, argv, "--audio-prebuffer-ms", 0, 1000, &settings->audio_prebuffer_ms) &&
          apply_cli_audio_codec(argc, argv, settings))) {
        return false;
    }
    return true;
}

static void native_config_apply_initial_desktop_hint(NativeSettings *settings) {
    if (!settings) {
        return;
    }
    settings->width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH;
    settings->height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT;
}

static bool native_config_validate_runtime(const NativeSettings *settings) {
    bool ok = true;
    if (settings->width == 0 || settings->height == 0 || settings->wheel_step == 0 ||
        settings->wheel_scroll_divisor == 0) {
        fprintf(stderr, "[native] invalid zero value in RDP config\n");
        ok = false;
    }
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        const NativeSessionConfig *session = &settings->sessions[slot];
        if (session->port == 0 || session->fps == 0) {
            fprintf(stderr, "[native] invalid zero value in %s session config\n", native_session_slot_name(slot));
            ok = false;
        }
    }
    return ok;
}

static bool native_session_config_validate_connect(const NativeSessionConfig *session, int slot, char *message,
                                                   size_t message_cap) {
    bool ok = true;
    const char *slot_name = native_session_slot_name(slot);
    if (!session->host[0]) {
        fprintf(stderr, "[native] missing RDP host for the %s session\n", slot_name);
        ok = false;
    }
    if (!session->username[0]) {
        fprintf(stderr, "[native] missing RDP username for the %s session\n", slot_name);
        ok = false;
    }
    if (!session->password[0]) {
        fprintf(stderr, "[native] missing RDP password for the %s session\n", slot_name);
        ok = false;
    }
    if (session->port == 0 || session->fps == 0) {
        fprintf(stderr, "[native] invalid zero value in %s session config\n", slot_name);
        ok = false;
    }
    if (!ok && message && message_cap > 0) {
        if (!session->host[0]) {
            (void)snprintf(message, message_cap, "Enter an IP address for the %s server.", slot_name);
        } else if (!session->username[0] || !session->password[0]) {
            (void)snprintf(message, message_cap, "Missing username or password for the %s server.", slot_name);
        } else {
            (void)snprintf(message, message_cap, "Invalid RDP config.");
        }
    }
    return ok;
}

static bool native_config_validate(const NativeSettings *settings) {
    return native_session_config_validate_connect(&settings->sessions[NATIVE_SESSION_SLOT_GREEN],
                                                  NATIVE_SESSION_SLOT_GREEN, NULL, 0) &&
           native_config_validate_runtime(settings);
}

static void native_config_log_effective(const NativeSettings *settings) {
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        const NativeSessionConfig *session = &settings->sessions[slot];
        if (slot != NATIVE_SESSION_SLOT_GREEN && !session->host[0]) {
            continue; /* unconfigured extra slot */
        }
        fprintf(stderr, "[native] effective %s RDP config host=%s port=%u username=%s password=%s domain=%s fps=%u\n",
                native_session_slot_name(slot), session->host, (unsigned)session->port,
                session->username[0] ? "set" : "missing", session->password[0] ? "set" : "missing",
                session->domain[0] ? "set" : "empty", (unsigned)session->fps);
    }
    fprintf(stderr,
            "[native] effective globals desktop=%ux%u wheelStep=%u wheelScrollDivisor=%u audioPrebufferMs=%u"
            " audioCodec=%s\n",
            (unsigned)settings->width, (unsigned)settings->height, (unsigned)settings->wheel_step,
            (unsigned)settings->wheel_scroll_divisor, (unsigned)settings->audio_prebuffer_ms,
            settings->audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto");
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

static const char *redact_if_sensitive(App *app, const char *line) {
    if (!line) {
        return "";
    }
    if (!app) {
        return line;
    }
    /* Runs on rdp-worker threads while the SDL thread may be rewriting ANOTHER slot's
     * config on a (re)Connect, so the scan takes redaction_lock (as does that write).
     * Every slot with a stored password is scanned, connected or not: checking slot->rdp
     * here would just race on that pointer, and redacting a disconnected slot's password
     * is at worst extra-safe. */
    const char *result = line;
    pthread_mutex_lock(&app->redaction_lock);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        const char *password = app->sessions[i].config.password;
        if (strlen(password) >= 4 && strstr(line, password)) {
            result = "[redacted sensitive detail]";
            break;
        }
    }
    pthread_mutex_unlock(&app->redaction_lock);
    return result;
}

/* Marks a slot's session as terminated. For the ACTIVE slot this mirrors the historical
 * single-session behavior (return to the pre-connect UI in interactive builds, exit the
 * process otherwise, always exit on native runtime failures). A BACKGROUND slot failure
 * must never take down the app: it is flagged and the SDL thread cleans that slot up. */
static void slot_stop_with_state(NativeSessionSlot *slot, RdpState state, int exit_code) {
    if (!slot) {
        return;
    }
    App *app = slot->app;
    atomic_store(&slot->terminal_state, (int)state);
    bool is_active = native_slot_is_active(slot);
    if (is_active && rdp_state_is_native_runtime_failure(state)) {
        /* Shared-decoder failures signal a platform problem; keep the historical
         * fail-fast so logs land before anything else goes wrong. */
        if (exit_code != 0) {
            atomic_store(&app->exit_code, exit_code);
        }
        atomic_store(&app->running, false);
        return;
    }
    if (!is_active || app->interactive_ui) {
        atomic_store(&slot->session_failed, true);
        return;
    }
    if (exit_code != 0) {
        atomic_store(&app->exit_code, exit_code);
    }
    atomic_store(&app->running, false);
}

static void on_state(void *ctx, RdpState state, const char *detail) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    App *app = slot ? slot->app : NULL;
    if (slot) {
        atomic_store(&slot->current_state, (int)state);
        /* Input arming/NumLock sync happen on the SDL thread, derived from this state
         * (see the per-tick block in the app loop): writing shared input state from a
         * worker here would race the SDL thread's switch sequence — a demoted slot's
         * stale set_active(false) landing after the switch would strand input off. */
    }

    const char *safe_detail = redact_if_sensitive(app, detail);
    fprintf(stderr, "[native] %s session state=%s(%d)%s%s\n",
            slot ? native_session_slot_name(slot->index) : "?", rdp_state_name(state), (int)state,
            safe_detail[0] ? " " : "", safe_detail);

    if (slot && rdp_state_is_terminal_error(state)) {
        fprintf(stderr, "[native] terminal native error on the %s session: %s\n",
                native_session_slot_name(slot->index), rdp_state_name(state));
        slot_stop_with_state(slot, state, rdp_state_exit_code(state));
    } else if (slot && state == RDP_STATE_STOPPED && atomic_load(&app->exit_code) == 0 &&
               !atomic_load(&slot->session_failed)) {
        /* Graceful server-side stop: flag it for the SDL thread (interactive builds go
         * back to the pre-connect UI or auto-switch; non-interactive builds exit). The
         * session_failed guard keeps the worker's final Stopped emission from
         * overwriting a terminal error already recorded for this slot — the UI must
         * report "failed: NetworkError", not "session stopped". */
        slot_stop_with_state(slot, state, 0);
    }
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
/* SDL_DestroyTexture() must run on the thread that owns the renderer#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
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
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    if (!native_slot_is_active(slot)) {
        /* Background session: only the active slot may touch the shared presentation. */
        return;
    }
    if (!rgba || len == 0 || width == 0 || height == 0) {
        app->decoder_errors++;
        slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }
    uint16_t desktop_width = (uint16_t)atomic_load(&slot->desktop_width);
    uint16_t desktop_height = (uint16_t)atomic_load(&slot->desktop_height);

    pthread_mutex_lock(&app->video_lock);
    if (!native_slot_is_active(slot)) {
        /* Demoted between the gate above and the lock: a switch tore the video path down
         * meanwhile, and this in-flight update must not resurrect it. */
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    if (app->video) {
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
        fprintf(stderr, "[native] switching graphics path from ss4s/H.264 to native RemoteFX RGBA\n");
        /* Closing the video track unloaded the shared pipeline; bring audio back. */
        native_reopen_audio_locked(app);
    }
    if (!app->rgba) {
        app->rgba = native_rgba_surface_open(desktop_width, desktop_height);
    } else if (native_rgba_surface_width(app->rgba) != desktop_width ||
               native_rgba_surface_height(app->rgba) != desktop_height) {
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
        native_defer_rgba_texture_destroy(app);
#endif
        if (native_rgba_surface_resize(app->rgba, desktop_width, desktop_height) != NATIVE_RGBA_OK) {
            native_rgba_surface_close(app->rgba);
            app->rgba = NULL;
        }
    }
    NativeRgbaResult result = app->rgba ? native_rgba_surface_apply(app->rgba, left, top, width, height, stride, rgba, len)
                                        : NATIVE_RGBA_NOMEM;
    pthread_mutex_unlock(&app->video_lock);

    if (result == NATIVE_RGBA_OK) {
        /* Bitmap frames satisfy the post-switch watchdog too, or a switch to a
         * RemoteFX-only server would reconnect despite frames arriving. */
        atomic_fetch_add(&slot->video_ok_frames, 1u);
    }
    if (result != NATIVE_RGBA_OK) {
        app->decoder_errors++;
        fprintf(stderr,
                "[native] terminal native error: DecoderError; RemoteFX bitmap update failed result=%d surface=%u rect=%ux%u+%u+%u\n",
                (int)result, (unsigned)surface_id, (unsigned)width, (unsigned)height, (unsigned)left, (unsigned)top);
        slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
}

static void on_log(void *ctx, const char *line) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    App *app = slot ? slot->app : NULL;
    fprintf(stderr, "[rdp:%s] %s\n", slot ? native_session_slot_name(slot->index) : "?",
            redact_if_sensitive(app, line));
}

/* Fires on the initial MCS/GCC handshake and again on every RDPGFX_RESET_GRAPHICS_PDU, so
 * the real EGFX graphics output size (which can differ from the negotiated session size)
 * stays current for both the ss4s/H.264 and RemoteFX RGBA paths and for pointer mapping. */
static void on_desktop_size(void *ctx, uint16_t width, uint16_t height) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    (void)slot->app;
    /* Only the slot's own atomics are written here. The input mapping derives from the
     * ACTIVE slot's atomics on the SDL thread once per tick: an is-active check followed
     * by a direct native_input_set_desktop_size would race a concurrent session switch —
     * this (then-active) worker could overwrite the size the switch installed for the
     * NEW slot, and that server may never send another size update. */
    atomic_store(&slot->desktop_width, width);
    atomic_store(&slot->desktop_height, height);
    fprintf(stderr, "[native] %s desktop=%ux%u\n", native_session_slot_name(slot->index), (unsigned)width,
            (unsigned)height);
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
    NativeSessionSlot *active = native_active_slot(app);
    if (viewport_width == 0) {
        viewport_width = (uint16_t)atomic_load(&active->desktop_width);
    }
    if (viewport_height == 0) {
        viewport_height = (uint16_t)atomic_load(&active->desktop_height);
    }
    app->media = native_media_open(viewport_width, viewport_height);
    return app->media;
}

/* ~40ms of PCM silence to prime a freshly (re)opened audio track: NDL defers the
 * pipeline's LOADCOMPLETED->PLAYING transition until audio data arrives, and the mixer
 * deliberately pauses while every session is silent — without the primer a reload during
 * a quiet moment stalls the video start until someone makes a sound (observed live:
 * PLAYING arrived 12s after LOADCOMPLETED). Kept minimal: everything primed here queues
 * in the hardware ahead of real audio, i.e. is direct A/V latency. Caller must hold
 * app->video_lock. */
static void native_prime_audio_silence_locked(App *app) {
    static const int16_t silence[NATIVE_MIXER_CHUNK_FRAMES * 2] = {0};
    if (!app || !app->audio || app->mixer_channels == 0 || app->mixer_channels > 2) {
        return;
    }
    size_t bytes = (size_t)NATIVE_MIXER_CHUNK_FRAMES * app->mixer_channels * sizeof(int16_t);
    for (int i = 0; i < 2; i++) {
        if (native_audio_feed(app->audio, (const uint8_t *)silence, bytes) != NATIVE_AUDIO_OK) {
            break;
        }
    }
    /* And keep paced silence flowing from the pump for a while: a one-shot primer is not
     * always enough for NDL to reach PLAYING when every session is silent. */
    native_mixer_feed_silence_for(&app->mixer, 2000u);
}

/* Mixer pump thread: pushes one mixed PCM chunk into the shared ss4s audio track. The
 * mixer lock is NOT held here (see audio_mixer.h), so taking video_lock is safe. */
static void native_mixer_feed_cb(void *ctx, const int16_t *samples, size_t frames) {
    App *app = (App *)ctx;
    pthread_mutex_lock(&app->video_lock);
    if (app->audio) {
        size_t bytes = frames * (size_t)app->mixer_channels * sizeof(int16_t);
        if (native_audio_feed(app->audio, (const uint8_t *)samples, bytes) == NATIVE_AUDIO_ERROR) {
            /* Mute instead of closing: on the webOS ss4s backends closing the audio track
             * unloads the shared pipeline and would freeze a live video stream. */
            fprintf(stderr, "[native-audio] mixed audio feed failed; muting audio\n");
            native_audio_disable(app->audio);
        }
    }
    pthread_mutex_unlock(&app->video_lock);
}

/* Initializes the mixer (once) at the given output format and starts its pump. Caller
 * must hold app->video_lock. */
static bool native_ensure_mixer_locked(App *app, uint32_t sample_rate, uint16_t channels) {
    if (!app || sample_rate == 0 || channels == 0) {
        return false;
    }
    if (app->mixer.initialized) {
        return true;
    }
    size_t prebuffer_frames = (size_t)app->audio_prebuffer_ms * sample_rate / 1000u;
    if (!native_mixer_init(&app->mixer, sample_rate, channels, NATIVE_MIXER_CHUNK_FRAMES,
                           NATIVE_MIXER_CAPACITY_FRAMES, prebuffer_frames)) {
        fprintf(stderr, "[native-audio] failed to initialize the audio mixer\n");
        return false;
    }
    app->mixer_sample_rate = sample_rate;
    app->mixer_channels = channels;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        /* Fresh mixers start at unity; restore the user's per-slot levels. */
        native_mixer_set_source_gain(&app->mixer, i, native_ui_mixer_gain_db_to_q15(app->mixer_gain_db[i]));
    }
    if (!native_mixer_pump_start(&app->mixer, native_mixer_feed_cb, app)) {
        native_mixer_destroy(&app->mixer);
        return false;
    }
    app->mixer_started = true;
    fprintf(stderr, "[native-audio] mixer running at %u Hz %u ch (per-source prebuffer %u ms)\n",
            (unsigned)sample_rate, (unsigned)channels, (unsigned)app->audio_prebuffer_ms);
    return true;
}

/* Opens the shared mixed-audio track speculatively as PCM 48kHz stereo before the
 * rdpsnd negotiation confirms it. The outcome is deterministic against
 * gnome-remote-desktop (the client advertises Opus 48k + PCM and grd prefers Opus, which
 * decodes to 48k stereo), and opening audio EARLY removes the track-open race entirely: both
 * tracks share one webOS hardware pipeline, so an audio open landing after the video
 * stream has started reloads the pipeline and stalls video until an IDR the server never
 * resends. If negotiation ends up choosing something else (or the server has no audio),
 * the normal on_audio_format path corrects or mutes it. Caller must hold app->video_lock. */
static void native_open_speculative_audio_locked(App *app) {
    if (!app || app->audio || !app->media) {
        return;
    }
    if (!native_ensure_mixer_locked(app, NATIVE_AUDIO_MIX_DEFAULT_RATE, NATIVE_AUDIO_MIX_DEFAULT_CHANNELS)) {
        return;
    }
    /* The mixer carries the jitter prebuffer per source; the track itself feeds with no
     * extra buffering (prebuffer 0). */
    app->audio = native_audio_open(app->media, RDP_AUDIO_CODEC_PCM_S16LE, app->mixer_sample_rate,
                                   app->mixer_channels, 0);
    if (app->audio) {
        fprintf(stderr, "[native-audio] opened speculative mixed PCM %uHz %uch track ahead of negotiation\n",
                (unsigned)app->mixer_sample_rate, (unsigned)app->mixer_channels);
        native_prime_audio_silence_locked(app);
    }
}

/* Reopens the audio track after something unloaded the shared media pipeline (on the
 * webOS ss4s backends closing either track does that). Cheap for audio: PCM needs no
 * keyframe, so sound resumes right after the reload. Caller must hold app->video_lock. */
static void native_reopen_audio_locked(App *app) {
    if (!app || !app->audio || !app->media) {
        return;
    }
    native_audio_close(app->audio);
    app->audio = native_audio_open(app->media, RDP_AUDIO_CODEC_PCM_S16LE, app->mixer_sample_rate,
                                   app->mixer_channels, 0);
    if (!app->audio) {
        fprintf(stderr, "[native-audio] failed to reopen audio after pipeline reload; continuing without audio\n");
    } else {
        native_prime_audio_silence_locked(app);
    }
}

static void on_video_au(void *ctx, const uint8_t *data, size_t len, bool is_keyframe, uint64_t pts90k) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    if (!native_slot_is_active(slot)) {
        /* Background session: drop its video outright. Normally the server has been asked
         * to suppress graphics; this also covers the in-flight tail and servers that
         * ignore TS_SUPPRESS_OUTPUT_PDU. */
        return;
    }
    if (!data || len == 0) {
        app->decoder_errors++;
        slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }

    /* The slot's desktop size reflects the server's real EGFX graphics output size
     * (on_desktop_size is re-invoked on every RDPGFX_RESET_GRAPHICS_PDU, not just the
     * initial MCS/GCC handshake), which can differ from the negotiated session size, e.g.
     * a TV whose hardware decoder always runs at panel resolution. Reopen the ss4s decoder
     * whenever that size changes so it matches what the server actually encodes. */
    uint16_t desktop_width = (uint16_t)atomic_load(&slot->desktop_width);
    uint16_t desktop_height = (uint16_t)atomic_load(&slot->desktop_height);
    pthread_mutex_lock(&app->video_lock);
    if (!native_slot_is_active(slot)) {
        /* Demoted between the gate above and the lock (see on_bitmap_update). */
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    if (app->rgba) {
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
        native_defer_rgba_texture_destroy(app);
#endif
        native_rgba_surface_close(app->rgba);
        app->rgba = NULL;
        fprintf(stderr, "[native] switching graphics path from native RemoteFX RGBA to ss4s/H.264\n");
    }
    if (app->video && (app->video_owner_slot != slot->index ||
                       app->video_owner_epoch != atomic_load(&slot->connect_epoch))) {
        /* The decoder still holds another stream's state (a switch left the old picture
         * up on purpose). Swap only once THIS stream's keyframe is in hand: deltas fed
         * into foreign decoder state would be garbage, and closing earlier would just
         * black the screen for the whole handover. */
        if (!is_keyframe) {
            pthread_mutex_unlock(&app->video_lock);
            unsigned waited = atomic_fetch_add(&slot->keyframe_wait_drops, 1u) + 1u;
            if (waited % 60u == 1u) {
                fprintf(stderr, "[native] waiting for a %s keyframe; %u delta AUs dropped since the switch\n",
                        native_session_slot_name(slot->index), waited);
            }
            return;
        }
        atomic_store(&slot->keyframe_wait_drops, 0u);
        if (desktop_width == native_video_width(app->video) &&
            desktop_height == native_video_height(app->video)) {
            /* Same dimensions: hand the running decoder over in-band. gnome-remote-desktop
             * sends SPS+PPS with every IDR, which restarts H.264 decode cleanly — no
             * pipeline reload, no black gap, and the mixed audio track is never touched. */
            fprintf(stderr, "[native] handing the video decoder to the %s session in-band (%ux%u)\n",
                    native_session_slot_name(slot->index), (unsigned)desktop_width, (unsigned)desktop_height);
            app->video_owner_slot = slot->index;
            app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            app->decoder_keyframe_pending = false;
        } else {
            fprintf(stderr, "[native] swapping video to the %s session (keyframe in hand, %ux%u -> %ux%u)\n",
                    native_session_slot_name(slot->index), (unsigned)native_video_width(app->video),
                    (unsigned)native_video_height(app->video), (unsigned)desktop_width, (unsigned)desktop_height);
            native_video_close(app->video);
            app->video = NULL;
            app->decoder_keyframe_pending = false;
            /* Closing the video track unloaded the shared pipeline; bring audio back. */
            native_reopen_audio_locked(app);
        }
    }
    if (app->video &&
        (desktop_width != native_video_width(app->video) || desktop_height != native_video_height(app->video))) {
        fprintf(stderr, "[native] ss4s surface size changed %ux%u -> %ux%u; reopening decoder\n",
                (unsigned)native_video_width(app->video), (unsigned)native_video_height(app->video),
                (unsigned)desktop_width, (unsigned)desktop_height);
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
        /* Closing the video track unloaded the shared pipeline; bring audio back.
         * The speculative open below won't (it no-ops while app->audio is set). */
        native_reopen_audio_locked(app);
    }
    if (!app->video) {
        NativeMedia *media = native_ensure_media_locked(app);
        if (media) {
            /* Audio must attach before the video track: the video open below triggers
             * the single pipeline load with the keyframe already in hand, whereas an
             * audio open arriving later would reload the pipeline mid-stream. */
            native_open_speculative_audio_locked(app);
            app->video = native_video_open(media, desktop_width, desktop_height, slot->config.fps);
            if (app->video) {
                app->video_owner_slot = slot->index;
                app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            }
        }
        if (!app->video) {
            pthread_mutex_unlock(&app->video_lock);
            app->decoder_errors++;
            fprintf(stderr, "[native] terminal native error: DecoderError; ss4s decoder unavailable\n");
            slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
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
        /* Signals the SDL thread that the freshly switched-to stream is decoding. */
        atomic_fetch_add(&slot->video_ok_frames, 1u);
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
    slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
}

/* Audio is strictly best-effort: any failure here logs and degrades to silence for that
 * session; neither handler ever stops a session (and must never call rdp_session_stop,
 * which would self-join the rdp-worker thread these callbacks run on). */
static void on_audio_format(void *ctx, uint32_t codec, uint32_t sample_rate, uint16_t channels) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    pthread_mutex_lock(&app->video_lock);
    if (codec != RDP_AUDIO_CODEC_PCM_S16LE && codec != RDP_AUDIO_CODEC_OPUS) {
        if (!slot->audio_incompatible_logged) {
            fprintf(stderr, "[native-audio] %s session negotiated unsupported codec %u; muting it in the mix\n",
                    native_session_slot_name(slot->index), (unsigned)codec);
            slot->audio_incompatible_logged = true;
        }
        native_mixer_set_source_open(&app->mixer, slot->index, false);
        slot->audio_routed = false;
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    if (!native_ensure_mixer_locked(app, sample_rate, channels)) {
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    if (sample_rate != app->mixer_sample_rate || channels != app->mixer_channels) {
        /* This session is negotiating AWAY from the current mix format, so it must not
         * count as a reason to keep the old pin: close its source first so the
         * any_routed scans below see only OTHER live sources. Otherwise a lone session
         * renegotiating 44.1<->48kHz would block its own re-pin and mute itself. */
        native_mixer_set_source_open(&app->mixer, slot->index, false);
        slot->audio_routed = false;
        bool any_routed = false;
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            any_routed = any_routed || app->sessions[i].audio_routed;
        }
        if (!any_routed) {
            /* Only the speculative default (PCM 44.1k) pinned the mixer so far; re-pin to
             * the actually negotiated format. The pump join must happen OUTSIDE
             * video_lock (its feed callback takes it), so drop and retake the lock and
             * revalidate: another session may have routed a source meanwhile.
             * mixer_repin_lock serializes the WHOLE stop->destroy->reinit against other
             * re-pinning workers: without it one worker can destroy (and memset) the
             * mixer's own control_lock while another still blocks inside pump_stop on
             * that same mutex — destroying a mutex with waiters is undefined behavior.
             * Lock order: mixer_repin_lock, then video_lock; the feed callback takes
             * only video_lock. */
            pthread_mutex_unlock(&app->video_lock);
            pthread_mutex_lock(&app->mixer_repin_lock);
            native_mixer_pump_stop(&app->mixer);
            pthread_mutex_lock(&app->video_lock);
            for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
                any_routed = any_routed || app->sessions[i].audio_routed;
            }
            if (!any_routed && app->mixer.initialized &&
                (sample_rate != app->mixer_sample_rate || channels != app->mixer_channels)) {
                fprintf(stderr, "[native-audio] re-pinning the mix from %uHz %uch to the negotiated %uHz %uch\n",
                        (unsigned)app->mixer_sample_rate, (unsigned)app->mixer_channels, (unsigned)sample_rate,
                        (unsigned)channels);
                native_mixer_destroy(&app->mixer);
                app->mixer_started = false;
                if (app->audio) {
                    /* The track runs at the old rate; drop it so the block below reopens
                     * it at the new one (with the usual pipeline-reload video dance). */
                    native_audio_close(app->audio);
                    app->audio = NULL;
                    if (app->video) {
                        fprintf(stderr,
                                "[native] audio format re-pin reloaded the media pipeline; reopening video on next keyframe\n");
                        native_video_close(app->video);
                        app->video = NULL;
                        app->decoder_keyframe_pending = false;
                        atomic_store(&app->video_refresh_needed, true);
                    }
                }
                if (!native_ensure_mixer_locked(app, sample_rate, channels)) {
                    pthread_mutex_unlock(&app->mixer_repin_lock);
                    pthread_mutex_unlock(&app->video_lock);
                    return;
                }
            } else if (app->mixer.initialized && !app->mixer.thread_running) {
                /* Lost the revalidation race (or nothing to re-pin): restart the pump for
                 * whichever format the mixer kept. */
                if (native_mixer_pump_start(&app->mixer, native_mixer_feed_cb, app)) {
                    app->mixer_started = true;
                }
            }
            pthread_mutex_unlock(&app->mixer_repin_lock);
        }
        if (sample_rate != app->mixer_sample_rate || channels != app->mixer_channels) {
            if (!slot->audio_incompatible_logged) {
                fprintf(stderr,
                        "[native-audio] %s session PCM %uHz %uch mismatches the %uHz %uch mix; muting it (no resampler)\n",
                        native_session_slot_name(slot->index), (unsigned)sample_rate, (unsigned)channels,
                        (unsigned)app->mixer_sample_rate, (unsigned)app->mixer_channels);
                slot->audio_incompatible_logged = true;
            }
            native_mixer_set_source_open(&app->mixer, slot->index, false);
            slot->audio_routed = false;
            pthread_mutex_unlock(&app->video_lock);
            return;
        }
    }

    if (codec == RDP_AUDIO_CODEC_OPUS) {
        /* Fresh stream (or renegotiation): restart decoder state. Safe against
         * on_audio_data — both run on this session's own rdp-worker thread. */
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = native_opus_decoder_open(sample_rate, channels);
        if (!slot->opus_decoder) {
            if (!slot->audio_incompatible_logged) {
                fprintf(stderr, "[native-audio] %s session: no Opus decoder available; muting it in the mix\n",
                        native_session_slot_name(slot->index));
                slot->audio_incompatible_logged = true;
            }
            native_mixer_set_source_open(&app->mixer, slot->index, false);
            slot->audio_routed = false;
            pthread_mutex_unlock(&app->video_lock);
            return;
        }
    } else if (slot->opus_decoder) {
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = NULL;
    }

    native_mixer_set_source_open(&app->mixer, slot->index, true);
    slot->audio_routed = true;
    slot->audio_incompatible_logged = false;
    fprintf(stderr, "[native-audio] %s session audio joined the mix (%s %uHz %uch)\n",
            native_session_slot_name(slot->index), codec == RDP_AUDIO_CODEC_OPUS ? "Opus->PCM" : "PCM",
            (unsigned)sample_rate, (unsigned)channels);

    if (!app->audio) {
        /* First working audio format and the speculative open didn't happen (or failed):
         * bring the shared track up now. */
        NativeMedia *media = native_ensure_media_locked(app);
        if (media) {
            app->audio = native_audio_open(media, RDP_AUDIO_CODEC_PCM_S16LE, app->mixer_sample_rate,
                                           app->mixer_channels, 0);
        }
        if (app->audio) {
            native_prime_audio_silence_locked(app);
            if (app->video) {
                /* First-time open under a live video stream reloads the shared pipeline
                 * (webOS); drop the dead video track so on_video_au reopens it on the next
                 * SPS+PPS+IDR instead of feeding P-frames into a fresh decoder — and ask
                 * the SDL thread to force that keyframe (gnome-remote-desktop never
                 * resends an IDR spontaneously). */
                fprintf(stderr,
                        "[native] audio open reloaded the media pipeline; reopening video on next keyframe\n");
                native_video_close(app->video);
                app->video = NULL;
                app->decoder_keyframe_pending = false;
                native_reopen_audio_locked(app);
                atomic_store(&app->video_refresh_needed, true);
            }
        } else {
            fprintf(stderr, "[native-audio] audio unavailable (PCM %uHz %uch); continuing with silent video\n",
                    (unsigned)sample_rate, (unsigned)channels);
        }
    }
    pthread_mutex_unlock(&app->video_lock);
}

static void on_audio_data(void *ctx, const uint8_t *data, size_t len, uint32_t ts_ms) {
    (void)ts_ms; /* the mixer pump provides the playback cadence */
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot || !data || len == 0 || !slot->audio_routed) {
        return;
    }
    App *app = slot->app;
    if (slot->opus_decoder) {
        const int16_t *pcm = NULL;
        int frames = native_opus_decoder_decode(slot->opus_decoder, data, len, &pcm);
        if (frames > 0) {
            (void)native_mixer_push(&app->mixer, slot->index, pcm, (size_t)frames);
        }
        return;
    }
    size_t frame_bytes = (size_t)app->mixer_channels * sizeof(int16_t);
    size_t frames = frame_bytes ? len / frame_bytes : 0;
    if (frames > 0) {
        /* Drop-oldest on overflow is handled (and logged) inside the mixer. */
        (void)native_mixer_push(&app->mixer, slot->index, (const int16_t *)(const void *)data, frames);
    }
}

static void on_pointer_bitmap(void *ctx, uint16_t width, uint16_t height, uint16_t hotspot_x,
                              uint16_t hotspot_y, const uint8_t *rgba, size_t len) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    /* Always cache into the slot's own cursor: a background session keeps its latest
     * shape current so a switch can re-apply it immediately. */
    native_cursor_submit_bitmap(&slot->cursor, width, height, hotspot_x, hotspot_y, rgba, len);
}

static void on_pointer_state(void *ctx, uint32_t state) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    native_cursor_submit_state(&slot->cursor, state);
}

static void on_pointer_position(void *ctx, uint16_t x, uint16_t y) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot || !native_slot_is_active(slot)) {
        return;
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    App *app = slot->app;
    atomic_store(&app->pointer_warp_x, (unsigned)x);
    atomic_store(&app->pointer_warp_y, (unsigned)y);
    atomic_store(&app->pointer_warp_pending, true);
#else
    (void)x;
    (void)y;
#endif
}

static bool native_any_slot_connected(const App *app) {
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        if (app->sessions[i].rdp) {
            return true;
        }
    }
    return false;
}

/* Tears down the shared presentation pipeline (RGBA surface, audio/video tracks, media
 * player). Sessions themselves are unaffected; call when no slot needs the screen. */
static void native_stop_media(App *app) {
    if (!app) {
        return;
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
    /* native_stop_media() only ever runs on the SDL/main thread, so it's safe to destroy
     * directly here rather than deferring; drain any texture a worker-thread callback
     * handed off but the render loop hasn't gotten to yet. */
    if (app->pending_texture_destroy) {
        SDL_DestroyTexture(app->pending_texture_destroy);
        app->pending_texture_destroy = NULL;
    }
#endif
    pthread_mutex_unlock(&app->video_lock);
}

/* Stops one slot's RDP session (joins its worker) and detaches it from the shared
 * input/mixer state. Does NOT touch the media pipeline — a switch keeps audio alive. */
static void native_stop_slot(App *app, int index) {
    if (!app || index < 0 || index >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    NativeSessionSlot *slot = &app->sessions[index];
    if (index == atomic_load(&app->active_index)) {
        native_input_set_active(&app->input, false);
        native_input_set_session(&app->input, NULL);
    }
    if (slot->rdp) {
        rdp_session_stop(slot->rdp);
        slot->rdp = NULL;
    }
    /* audio_routed is scanned by OTHER slots' workers under video_lock when they decide
     * whether the mixer may be re-pinned; clear it under the same lock or a concurrent
     * format negotiation can see this stopped slot as still routed and mute itself
     * instead of re-pinning. (The join above means this slot's own worker is gone.) */
    pthread_mutex_lock(&app->video_lock);
    native_mixer_set_source_open(&app->mixer, index, false);
    slot->audio_routed = false;
    slot->audio_incompatible_logged = false;
    pthread_mutex_unlock(&app->video_lock);
    /* The worker is joined above; its decoder can be freed from this thread now. */
    native_opus_decoder_close(slot->opus_decoder);
    slot->opus_decoder = NULL;
    slot->suppressed = false;
    atomic_store(&slot->current_state, (int)RDP_STATE_IDLE);
    atomic_store(&slot->session_failed, false);
}

static void native_stop_all_sessions(App *app) {
    if (!app) {
        return;
    }
#if HELLOLG_WITH_EVDEV_INPUT
    /* Release the global evdev grab so the compositor/preconnect UI gets USB input back. */
    native_evdev_input_stop(&app->evdev_input);
#endif
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_stop_slot(app, i);
    }
    native_stop_media(app);
}

/* (Re)connects one slot using slot->config (the caller copies the settings in first).
 * Only the ACTIVE slot resets the shared decoder/input state; a background
 * connect-on-demand must not disturb the running stream. */
static bool native_slot_connect(App *app, int index) {
    if (!app || index < 0 || index >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }
    NativeSessionSlot *slot = &app->sessions[index];
    native_stop_slot(app, index);
    atomic_fetch_add(&slot->connect_epoch, 1u);
    atomic_store(&slot->terminal_state, (int)RDP_STATE_IDLE);
    atomic_store(&slot->desktop_width, NATIVE_RDP_INITIAL_DESKTOP_WIDTH);
    atomic_store(&slot->desktop_height, NATIVE_RDP_INITIAL_DESKTOP_HEIGHT);

    bool is_active = index == atomic_load(&app->active_index);
    if (is_active) {
        app->decoder_errors = 0;
        app->decoder_keyframe_pending = false;
        app->video_plane_punched = false;
        atomic_store(&app->exit_code, 0);
        uint16_t window_width = (uint16_t)atomic_load(&app->input.window_width);
        uint16_t window_height = (uint16_t)atomic_load(&app->input.window_height);
        native_input_init(&app->input, NULL, NATIVE_RDP_INITIAL_DESKTOP_WIDTH, NATIVE_RDP_INITIAL_DESKTOP_HEIGHT);
        if (window_width != 0 && window_height != 0) {
            native_input_set_window_size(&app->input, window_width, window_height);
        }
    }

    RdpConfig config = {
        .host = slot->config.host,
        .port = slot->config.port,
        .username = slot->config.username,
        .password = slot->config.password,
        .domain = slot->config.domain,
        .width = NATIVE_RDP_INITIAL_DESKTOP_WIDTH,
        .height = NATIVE_RDP_INITIAL_DESKTOP_HEIGHT,
        .fps = slot->config.fps,
        .prefer_pcm_audio = app->audio_codec == NATIVE_AUDIO_CODEC_PCM ? 1 : 0,
    };
    RdpCallbacks callbacks = {
        .ctx = slot,
        .on_state = on_state,
        .on_log = on_log,
        .on_desktop_size = on_desktop_size,
        .on_bitmap_update = on_bitmap_update,
        .on_video_au = on_video_au,
        .on_audio_format = on_audio_format,
        .on_audio_data = on_audio_data,
        .on_pointer_bitmap = on_pointer_bitmap,
        .on_pointer_position = on_pointer_position,
        .on_pointer_state = on_pointer_state,
    };

    fprintf(stderr, "[native] starting %s session for %s:%u (%ux%u@%u AVC420/RemoteFX)\n",
            native_session_slot_name(index), config.host, (unsigned)config.port, (unsigned)config.width,
            (unsigned)config.height, (unsigned)config.fps);
    slot->rdp = rdp_session_start(&config, &callbacks);
    if (!slot->rdp) {
        fprintf(stderr, "[native] rdp_session_start failed for the %s session\n", native_session_slot_name(index));
        return false;
    }
    if (is_active) {
        native_input_set_session(&app->input, slot->rdp);
    }
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

static void native_drain_pointer_warp(App *app, SDL_Window *window) {
    if (!app || !window || !atomic_exchange(&app->pointer_warp_pending, false)) {
        return;
    }
    uint16_t dw = (uint16_t)atomic_load(&app->input.desktop_width);
    uint16_t dh = (uint16_t)atomic_load(&app->input.desktop_height);
    uint16_t ww = (uint16_t)atomic_load(&app->input.window_width);
    uint16_t wh = (uint16_t)atomic_load(&app->input.window_height);
    unsigned x = atomic_load(&app->pointer_warp_x);
    unsigned y = atomic_load(&app->pointer_warp_y);
    int wx = (dw != 0 && ww != 0) ? (int)((x * (unsigned)ww) / (unsigned)dw) : (int)x;
    int wy = (dh != 0 && wh != 0) ? (int)((y * (unsigned)wh) / (unsigned)dh) : (int)y;
    /* The mouse is read from grabbed evdev, so there is no SDL_MOUSEMOTION echo to update
     * virtual_mouse_x/y after the warp. Set it to the warp target directly so the next
     * relative delta integrates from here (otherwise the first move snaps the cursor back),
     * then warp the real pointer so the server cursor on the platform plane follows. */
    native_set_virtual_mouse_position(app, wx, wy);
    SDL_WarpMouseInWindow(window, wx, wy);
}

/* Apply pending server cursor shape/visibility on the SDL thread (cheap when idle). */
static void native_cursor_tick(App *app) {
    if (!app) {
        return;
    }
    native_cursor_apply(&native_active_slot(app)->cursor, (uint16_t)atomic_load(&app->input.desktop_width),
                        (uint16_t)atomic_load(&app->input.desktop_height),
                        (uint16_t)atomic_load(&app->input.window_width),
                        (uint16_t)atomic_load(&app->input.window_height));
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

/* Color buttons on the TV remote select the session slot with the same name, in the
 * remote's own order: red, green, yellow, blue. The remote is deliberately NOT
 * evdev-grabbed, so these arrive as SDL scancodes (sysroot SDL_webOS.h; 486..489 are the
 * literal values for builds without that header). */
static int native_sdl_webos_color_slot(const SDL_KeyboardEvent *event) {
    if (!event) {
        return -1;
    }
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_RED || event->keysym.scancode == 486) {
        return NATIVE_SESSION_SLOT_RED;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_GREEN || event->keysym.scancode == 487) {
        return NATIVE_SESSION_SLOT_GREEN;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_YELLOW || event->keysym.scancode == 488) {
        return NATIVE_SESSION_SLOT_YELLOW;
    }
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_BLUE || event->keysym.scancode == 489) {
        return NATIVE_SESSION_SLOT_BLUE;
    }
#else
    if (event->keysym.scancode == 486) {
        return NATIVE_SESSION_SLOT_RED;
    }
    if (event->keysym.scancode == 487) {
        return NATIVE_SESSION_SLOT_GREEN;
    }
    if (event->keysym.scancode == 488) {
        return NATIVE_SESSION_SLOT_YELLOW;
    }
    if (event->keysym.scancode == 489) {
        return NATIVE_SESSION_SLOT_BLUE;
    }
#endif
    return -1;
}

static void native_request_session_switch(App *app, int target);
/* Volume-mixer overlay key hooks for the evdev drain (definitions live with the overlay
 * code, after the presenters they use). */
static bool native_mixer_overlay_consumes_evdev_key(uint16_t code);
static void native_mixer_overlay_evdev_key(App *app, uint16_t code);

/* Pre-connect screen: swallow the color keys before the LVGL key queue eats them. They
 * carry the same navigation semantics as on the streaming screen — back to that slot's
 * live stream when it is connected, otherwise its configurator form (which is what the
 * next Connect targets). App exit is the system's business (webOS EXIT/home). */
static int native_filter_webos_system_keys(void *userdata, SDL_Event *event) {
    App *app = (App *)userdata;
    if (!event || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)) {
        return 1;
    }
    int slot = native_sdl_webos_color_slot(&event->key);
    if (slot >= 0) {
        if (event->type == SDL_KEYDOWN && app) {
            native_request_session_switch(app, slot);
        }
        return 0;
    }
    return 1;
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

/* Track which mouse buttons / key scancodes we have sent a down for but not yet an up, so
 * native_flush_held_inputs() can release them to the server before the evdev grab is dropped
 * on focus loss. Called right after each forwarded button/key event (evdev drains and the SDL
 * fallback). SDL thread only. */
static void native_track_button(App *app, NativeInputButton button, bool down) {
    if (app && button >= NATIVE_INPUT_BUTTON_LEFT && button <= NATIVE_INPUT_BUTTON_MIDDLE) {
        app->held_mouse[button - 1] = down;
    }
}

static void native_track_key(App *app, uint8_t scancode, bool extended, bool down) {
    if (!app) {
        return;
    }
    unsigned idx = (unsigned)scancode | (extended ? 0x100u : 0u);
    if (down) {
        app->held_keys[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
    } else {
        app->held_keys[idx / 8u] &= (uint8_t) ~(1u << (idx % 8u));
    }
}

/* Send an up for every input still held down, so a focus loss mid-press does not strand the
 * server in a drag or an auto-repeating key once we release the grab. Runs on the SDL thread
 * while the session is still active (focus loss / background, not session teardown). */
static void native_flush_held_inputs(App *app) {
    if (!app) {
        return;
    }
    int wx = atomic_load(&app->virtual_mouse_x);
    int wy = atomic_load(&app->virtual_mouse_y);
    for (int b = 0; b < 3; b++) {
        if (app->held_mouse[b]) {
            app->held_mouse[b] = false;
            native_input_pointer_button(&app->input, wx, wy, (NativeInputButton)(b + 1), false);
        }
    }
    for (unsigned idx = 0; idx < 512u; idx++) {
        if (app->held_keys[idx / 8u] & (uint8_t)(1u << (idx % 8u))) {
            app->held_keys[idx / 8u] &= (uint8_t) ~(1u << (idx % 8u));
            native_input_key(&app->input, (uint8_t)(idx & 0xffu), false, (idx & 0x100u) != 0u);
        }
    }
}

#if HELLOLG_WITH_EVDEV_INPUT
#define NATIVE_EVDEV_MOUSE_BATCH 64u
#define NATIVE_EVDEV_KEYBOARD_BATCH 64u

static void native_flush_pending_evdev_motion(App *app, int *pending_dx, int *pending_dy, bool *moved) {
    if (!app || !pending_dx || !pending_dy || (*pending_dx == 0 && *pending_dy == 0)) {
        return;
    }
    native_set_virtual_mouse_position(app, atomic_load(&app->virtual_mouse_x) + *pending_dx,
                                      atomic_load(&app->virtual_mouse_y) + *pending_dy);
    native_input_pointer_move(&app->input, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
    *pending_dx = 0;
    *pending_dy = 0;
    if (moved) {
        *moved = true;
    }
}

/* SDL thread: apply queued raw-evdev mouse events. Relative motion is integrated into the
 * logical pointer and sent as an absolute server position; buttons/wheel are sent at the
 * current position. Because the mouse is grabbed, the compositor no longer moves the OS
 * pointer, so we warp it to the logical position to make the server cursor (drawn on the
 * platform cursor plane) follow. */
static void native_drain_evdev_mouse(App *app, SDL_Window *window) {
    if (!native_evdev_input_mouse_active(&app->evdev_input)) {
        return;
    }
    bool moved = false;
    int pending_dx = 0;
    int pending_dy = 0;
    NativeMouseEv events[NATIVE_EVDEV_MOUSE_BATCH];
    for (;;) {
        size_t count = native_evdev_input_pop_mouse_batch(&app->evdev_input, events, NATIVE_EVDEV_MOUSE_BATCH);
        if (count == 0) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            NativeMouseEv *ev = &events[i];
            switch (ev->kind) {
            case NATIVE_MOUSE_EV_MOTION:
                pending_dx += ev->dx;
                pending_dy += ev->dy;
                break;
            case NATIVE_MOUSE_EV_BUTTON: {
                native_flush_pending_evdev_motion(app, &pending_dx, &pending_dy, &moved);
                NativeInputButton button = native_button_from_sdl(ev->sdl_button);
                if (button != 0) {
                    native_input_pointer_button(&app->input, atomic_load(&app->virtual_mouse_x),
                                                atomic_load(&app->virtual_mouse_y), button, ev->down);
                    native_track_button(app, button, ev->down);
                }
                break;
            }
            case NATIVE_MOUSE_EV_WHEEL:
                native_flush_pending_evdev_motion(app, &pending_dx, &pending_dy, &moved);
                if (ev->wheel_y != 0) {
                    native_send_scaled_wheel(app, ev->wheel_y);
                }
                break;
            }
        }
    }
    native_flush_pending_evdev_motion(app, &pending_dx, &pending_dy, &moved);
    if (moved) {
        if (app->cursor_reassert_pending) {
            /* First real movement after regaining focus: re-show the cursor now that the
             * visibility call coincides with genuine pointer activity (webOS ignored it at
             * bare focus-gain, e.g. when the overlay was dismissed with Esc). */
            native_cursor_reassert(&native_active_slot(app)->cursor);
            app->cursor_reassert_pending = false;
        }
        if (window) {
            int wx = atomic_load(&app->virtual_mouse_x);
            int wy = atomic_load(&app->virtual_mouse_y);
            SDL_WarpMouseInWindow(window, wx, wy);
        }
    }
}

static void native_log_unmapped_evdev_key(uint16_t code, bool down) {
    static unsigned unmapped_log_count = 0;
    if (unmapped_log_count >= 256) {
        if (unmapped_log_count == 256) {
            fprintf(stderr, "[native-input] further unmapped evdev key logs suppressed\n");
            unmapped_log_count++;
        }
        return;
    }
    unmapped_log_count++;
    fprintf(stderr, "[native-input] unmapped evdev key %s code=%u\n", down ? "down" : "up", (unsigned)code);
}

/* SDL thread: apply queued raw-evdev keyboard events. Each Linux keycode is translated to its
 * RDP set-1 scancode (+E0 flag) and injected as a scancode key event; there is no unicode/IME
 * path (the grabbed keyboard delivers physical scancodes only), so no TEXTINPUT suppression is
 * needed. Unlike the SDL path, typing does NOT hide the pointer: with the mouse grabbed we
 * own the cursor outright, so its visibility follows the RDP server's pointer state alone. A
 * local hide-on-keypress would only fight that (and is what made the cursor vanish mid-typing
 * before). */
/* The Magic Remote's virtual input nodes ("Smart Remote RCU Input", "LGE Network
 * Input") qualify as relative mice and therefore ARE evdev-grabbed during streaming, so
 * the remote's color keys arrive here as Linux key codes instead of reaching SDL through
 * the compositor. Map them to session-slot navigation, same as the SDL scancode path. */
static int native_evdev_color_slot(uint16_t code) {
    switch (code) {
    case 398: /* KEY_RED */
        return NATIVE_SESSION_SLOT_RED;
    case 399: /* KEY_GREEN */
        return NATIVE_SESSION_SLOT_GREEN;
    case 400: /* KEY_YELLOW */
        return NATIVE_SESSION_SLOT_YELLOW;
    case 401: /* KEY_BLUE */
        return NATIVE_SESSION_SLOT_BLUE;
    default:
        return -1;
    }
}

static void native_drain_evdev_keyboard(App *app) {
    if (!native_evdev_input_keyboard_active(&app->evdev_input)) {
        return;
    }
    NativeKeyboardEv events[NATIVE_EVDEV_KEYBOARD_BATCH];
    for (;;) {
        size_t count =
            native_evdev_input_pop_keyboard_batch(&app->evdev_input, events, NATIVE_EVDEV_KEYBOARD_BATCH);
        if (count == 0) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            int color_slot = native_evdev_color_slot(events[i].code);
            if (color_slot >= 0) {
                if (events[i].down) {
                    /* May stop evdev input from under this drain (the configurator and
                     * mixer-overlay paths release the grab). That is safe by
                     * construction: the rest of THIS batch lives in the local array, a
                     * repeated stop early-returns on !lock_initialized, and the next
                     * pop_keyboard_batch returns 0 via its !started guard without
                     * touching the destroyed lock. */
                    native_request_session_switch(app, color_slot);
                }
                continue;
            }
            if (app->mixer_overlay_visible && native_mixer_overlay_consumes_evdev_key(events[i].code)) {
                if (events[i].down) {
                    native_mixer_overlay_evdev_key(app, events[i].code);
                }
                continue;
            }
            uint8_t rdp_scancode = 0;
            bool extended = false;
            if (!native_input_linux_keycode_to_rdp(events[i].code, &rdp_scancode, &extended)) {
                native_log_unmapped_evdev_key(events[i].code, events[i].down);
                continue;
            }
            bool sent = native_input_key(&app->input, rdp_scancode, events[i].down, extended);
            if (sent) {
                native_track_key(app, rdp_scancode, extended, events[i].down);
            }
        }
    }
}
#endif

static void native_wait_for_loop_tick(App *app, uint32_t delay_ms) {
    uint32_t timeout_ms = delay_ms == 0 ? 1u : delay_ms;
#if HELLOLG_WITH_EVDEV_INPUT
    int wake_fd = app ? native_evdev_input_wake_fd(&app->evdev_input) : -1;
    if (wake_fd >= 0) {
        /* Input can wake the loop before the full frame timeout so a keystroke/click is not
         * gated by the render sleep. But a 500-1000 Hz USB mouse would otherwise wake it on
         * every report and spin the whole loop (UI tick, present checks, SDL pump) at report
         * rate. So once input has arrived, process it after at most min_interval_ms (bounding
         * added latency to half a frame and the loop to ~2x/frame); while idle, wait the full
         * frame timeout as before. Bursty input coalesces in the reader's ring meanwhile. */
        const uint32_t min_interval_ms = timeout_ms > 2u ? timeout_ms / 2u : 0u;
        uint32_t start = SDL_GetTicks();
        bool woke = false;
        for (;;) {
            uint32_t elapsed = SDL_GetTicks() - start;
            uint32_t deadline_ms = woke ? min_interval_ms : timeout_ms;
            if (elapsed >= deadline_ms) {
                return; /* deadline reached: run the loop (processing any coalesced input) */
            }
            struct pollfd pfd;
            pfd.fd = wake_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int ret;
            do {
                ret = poll(&pfd, 1, (int)(deadline_ms - elapsed));
            } while (ret < 0 && errno == EINTR);
            if (ret <= 0) {
                return; /* deadline elapsed (or poll error): run the loop */
            }
            if (!(pfd.revents & POLLIN)) {
                return; /* unexpected condition on the wake fd (POLLERR/HUP): don't spin on it */
            }
            native_evdev_input_clear_wake(&app->evdev_input);
            /* First input this wait: switch to the min-interval deadline so it is processed
             * promptly; further wakes before then just coalesce. */
            woke = true;
        }
    }
#else
    (void)app;
#endif
    SDL_Delay((Uint32)timeout_ms);
}

typedef enum NativeInputStartResult {
    NATIVE_INPUT_START_OK,          /* keyboard reader is active */
    NATIVE_INPUT_START_NO_KEYBOARD, /* readers up (or nothing attached) but no keyboard to grab */
    NATIVE_INPUT_START_UNAVAILABLE, /* the input subsystem itself could not start (eventfd/thread) */
} NativeInputStartResult;

/* Start the evdev input readers when streaming becomes active. The mouse degrades to the SDL
 * compositor-pointer path (Magic Remote) when no USB mouse is present, so a failed mouse grab
 * is informational; a failed keyboard grab means NO keyboard input (there is no SDL keyboard
 * fallback), so it is a loud warning. Returns NATIVE_INPUT_START_OK when the keyboard reader is
 * active, NATIVE_INPUT_START_NO_KEYBOARD when there is simply no keyboard to grab, and
 * NATIVE_INPUT_START_UNAVAILABLE when the reader could not start at all (so callers do not
 * misreport a resource failure as an absent keyboard). */
static NativeInputStartResult native_start_streaming_input(App *app) {
    if (!app) {
        return NATIVE_INPUT_START_UNAVAILABLE;
    }
#if HELLOLG_WITH_EVDEV_INPUT
    if (!native_evdev_input_start(&app->evdev_input)) {
        /* The reader failed to come up; the grab path cannot tell us whether a keyboard exists.
         * Probe /dev/input directly so an eventfd/thread failure with a keyboard attached is
         * reported as a resource failure, not "no keyboard detected". */
        if (native_evdev_input_probe_keyboard()) {
            fprintf(stderr, "[native] WARNING: input capture failed to start even though a USB keyboard is "
                            "attached; this session has no mouse or keyboard input\n");
            return NATIVE_INPUT_START_UNAVAILABLE;
        }
        fprintf(stderr,
                "[native] WARNING: no USB mouse/keyboard grabbed; using SDL mouse fallback and no keyboard input\n");
        return NATIVE_INPUT_START_NO_KEYBOARD;
    }
    if (!native_evdev_input_mouse_active(&app->evdev_input)) {
        fprintf(stderr, "[native] no USB mouse to grab; using the compositor pointer (Magic Remote) via SDL\n");
    }
    if (!native_evdev_input_keyboard_active(&app->evdev_input)) {
        fprintf(stderr,
                "[native] WARNING: no USB keyboard grabbed and there is no SDL keyboard fallback; this "
                "session has no keyboard input until a USB keyboard is attached (it is picked up live)\n");
        return NATIVE_INPUT_START_NO_KEYBOARD;
    }
    return NATIVE_INPUT_START_OK;
#else
    return NATIVE_INPUT_START_UNAVAILABLE;
#endif
}

/* Release the evdev grabs (mouse + keyboard). Safe when nothing is grabbed (idempotent). Used
 * on focus loss / backgrounding: EVIOCGRAB is a GLOBAL capture, so a running-but-unfocused app
 * must not keep holding it or the webOS home UI / TV overlay menus get no mouse or keyboard. */
static void native_stop_streaming_input(App *app) {
    if (!app) {
        return;
    }
    /* Flush releases for anything held BEFORE dropping the grab: once the grab is gone the
     * up-events go to the overlay, not RDP, and the server would keep the drag/key stuck. */
    native_flush_held_inputs(app);
#if HELLOLG_WITH_EVDEV_INPUT
    native_evdev_input_stop(&app->evdev_input);
#endif
}

/* Re-acquire input after regaining focus / returning to foreground, but only on the streaming
 * screen (the preconnect UI needs the SDL mouse, so we must not grab there). Besides re-grabbing,
 * re-assert the server cursor and re-home the OS pointer: a webOS overlay leaves the platform
 * pointer hidden behind our back, so without this the RDP cursor stays invisible until a big
 * mouse sweep. */
static void native_resume_streaming_input(App *app, SDL_Window *window) {
    if (!app || atomic_load(&native_active_slot(app)->current_state) != (int)RDP_STATE_ACTIVE) {
        return;
    }
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    if (app->mixer_overlay_visible) {
        /* The overlay deliberately released the grab so SDL delivers fader clicks;
         * re-grabbing on focus regain would steal the pointer back from it. Its hide
         * path re-grabs (focus-gated) once it closes. */
        return;
    }
#endif
    (void)native_start_streaming_input(app);
    native_cursor_reassert(&native_active_slot(app)->cursor);
    /* Also re-assert on the next real mouse movement — see cursor_reassert_pending. */
    app->cursor_reassert_pending = true;
    if (window) {
        SDL_WarpMouseInWindow(window, atomic_load(&app->virtual_mouse_x), atomic_load(&app->virtual_mouse_y));
    }
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
        slot_stop_with_state(native_active_slot(app), RDP_STATE_DECODER_ERROR,
                             rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
    return status;
}

static void native_set_slot_badge_color(SDL_Renderer *renderer, int slot) {
    switch (slot) {
    case NATIVE_SESSION_SLOT_RED:
        SDL_SetRenderDrawColor(renderer, 255, 65, 54, 230);
        break;
    case NATIVE_SESSION_SLOT_GREEN:
        SDL_SetRenderDrawColor(renderer, 46, 204, 64, 230);
        break;
    case NATIVE_SESSION_SLOT_YELLOW:
        SDL_SetRenderDrawColor(renderer, 255, 220, 0, 230);
        break;
    default:
        SDL_SetRenderDrawColor(renderer, 0, 116, 217, 230); /* blue */
        break;
    }
}

/* Colored square in the top-right corner naming the active session for a short moment
 * after a switch. Drawn over the transparent hole-punch frame, so the hardware video
 * plane stays visible around it; presented once (latch) and cleared back to the punch
 * frame on expiry. Skipped while the RemoteFX RGBA path owns the renderer. */
static void native_present_indicator_frame(App *app, SDL_Renderer *renderer) {
    if (app->indicator_slot < 0) {
        return;
    }
    if (SDL_TICKS_PASSED(SDL_GetTicks(), app->indicator_until_ticks)) {
        app->indicator_slot = -1;
        app->indicator_drawn = false;
        /* Re-arm the punch latch so the next tick clears the square away. */
        app->video_plane_punched = false;
        return;
    }
    if (app->indicator_drawn) {
        return;
    }
    int output_width = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, NULL) != 0) {
        output_width = NATIVE_LOCAL_SURFACE_WIDTH;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    native_set_slot_badge_color(renderer, app->indicator_slot);
    SDL_Rect badge = {output_width - 72, 24, 48, 48};
    SDL_RenderFillRect(renderer, &badge);
    SDL_RenderPresent(renderer);
    app->indicator_drawn = true;
    /* The frame on screen is not the plain punch frame anymore. */
    app->video_plane_punched = true;
}

/* Opaque splash covering the video plane while a swap is in flight: the pipeline reload
 * window (track close -> load -> PLAYING) would otherwise show through as raw black.
 * Drawn once (latch); a ~300ms grace keeps in-band handovers splash-free. */
static void native_present_switch_splash(App *app, SDL_Renderer *renderer) {
    if (app->switch_splash_drawn) {
        return;
    }
    uint32_t switch_started = app->switch_deadline_ticks - NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
    if (!SDL_TICKS_PASSED(SDL_GetTicks(), switch_started + 300u)) {
        return; /* grace: quick handovers finish before the splash would flash */
    }
    int output_width = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, NULL) != 0) {
        output_width = NATIVE_LOCAL_SURFACE_WIDTH;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 16, 20, 24, 255);
    SDL_RenderClear(renderer);
    native_set_slot_badge_color(renderer, atomic_load(&app->active_index));
    SDL_Rect badge = {output_width - 72, 24, 48, 48};
    SDL_RenderFillRect(renderer, &badge);
    SDL_RenderPresent(renderer);
    app->switch_splash_drawn = true;
    app->video_plane_punched = true; /* screen content is ours, not the punch frame */
}

/* ---- Volume-mixer overlay (SDL thread only) ----
 * A semi-transparent panel over the live stream with one vertical slider per session
 * slot, in that slot's badge color. Opened by re-pressing the ACTIVE slot's color button
 * (that press used to just re-flash the badge); volume changes hit the mixer source
 * immediately. Up/down move the selected slider, left/right (or another slot's color
 * key) change the selection, the active slot's color key / OK / Back close it, and it
 * auto-hides after NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS without a key. */

static void native_show_session_indicator(App *app, int slot);

/* Any interaction: repaint and push the auto-hide deadline out. */
static void native_mixer_overlay_touch(App *app) {
    app->mixer_overlay_hide_ticks = SDL_GetTicks() + NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS;
    app->mixer_overlay_drawn = false;
}

static void native_mixer_overlay_hide(App *app) {
    if (!app->mixer_overlay_visible) {
        return;
    }
    app->mixer_overlay_visible = false;
    app->mixer_overlay_drawn = false;
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    native_ui_mixer_hide(native_preconnect_ui_mixer(app->preconnect_ui));
    app->mixer_overlay_dragging = false;
    /* Hand the pointer back to the stream: re-grab and restore the session's cursor —
     * but only while we are actually staying on a live FOCUSED stream. A hide on the way
     * to a configurator or a failure path must not re-grab (those screens own the SDL
     * mouse), and neither must an auto-hide under a webOS system overlay: grabbing there
     * would steal input from the system menu — focus regain re-grabs instead. The
     * per-tick arming derive re-arms RDP input next tick. */
    if (app->streaming_visible && !app->window_unfocused &&
        atomic_load(&native_active_slot(app)->current_state) == (int)RDP_STATE_ACTIVE) {
        (void)native_start_streaming_input(app);
        native_cursor_reassert(&native_active_slot(app)->cursor);
        /* The compositor pointer sits wherever the fader interaction left it while
         * virtual_mouse_x/y still hold the RDP position (overlay motion is consumed):
         * warp back like native_resume_streaming_input does, or the visible cursor
         * jumps on the first post-close delta. */
        SDL_Window *focus_window = SDL_GetKeyboardFocus();
        if (focus_window) {
            SDL_WarpMouseInWindow(focus_window, atomic_load(&app->virtual_mouse_x),
                                  atomic_load(&app->virtual_mouse_y));
        }
    }
#endif
    /* Re-arm the punch latch so the next tick clears the panel away. */
    app->video_plane_punched = false;
}

static void native_mixer_overlay_show(App *app) {
    pthread_mutex_lock(&app->video_lock);
    bool rgba_active = app->rgba != NULL;
    pthread_mutex_unlock(&app->video_lock);
    if (rgba_active) {
        /* The RemoteFX RGBA path repaints the whole renderer every frame, so the panel
         * would be invisible while its keys still hijack input: keep the badge flash. */
        native_show_session_indicator(app, atomic_load(&app->active_index));
        return;
    }
    app->mixer_overlay_visible = true;
    app->mixer_overlay_selected = atomic_load(&app->active_index);
    /* A still-armed badge would hold its drawn latch through our hide and leave a stale
     * panel on screen until it expires; the overlay supersedes it. */
    app->indicator_slot = -1;
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    native_ui_mixer_show(native_preconnect_ui_mixer(app->preconnect_ui));
    /* Hand the pointer to the SYSTEM while the panel is up: drop the evdev grab so the
     * compositor drives a visible cursor again, disarm RDP input (the per-tick derive
     * keeps it off while the overlay is visible) and show the plain arrow without
     * touching the session's cached cursor shape. Faders become click/drag targets. */
    app->mixer_overlay_dragging = false;
    native_stop_streaming_input(app);
    native_input_set_active(&app->input, false);
    native_cursor_show_default();
#endif
    native_mixer_overlay_touch(app);
}

static void native_mixer_overlay_select(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        app->mixer_overlay_selected = slot;
    }
    native_mixer_overlay_touch(app); /* even an edge bump keeps the overlay alive */
}

static void native_mixer_overlay_set_db(App *app, int slot, int gain_db) {
    if (slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    if (gain_db < NATIVE_MIXER_FADER_MIN_DB) {
        gain_db = NATIVE_MIXER_FADER_MIN_DB;
    }
    if (gain_db > NATIVE_MIXER_FADER_MAX_DB) {
        gain_db = NATIVE_MIXER_FADER_MAX_DB;
    }
    if (gain_db != (int)app->mixer_gain_db[slot]) {
        /* Workers re-read this set in native_ensure_mixer_locked (under video_lock) on a
         * mixer re-pin; the write takes the same lock. The unguarded read above is fine:
         * the SDL thread is the only writer. The live-gain call stays under video_lock
         * too — a worker may be destroying/reinitializing app->mixer during a re-pin,
         * and touching its internal mutex mid-destroy is UB. */
        pthread_mutex_lock(&app->video_lock);
        app->mixer_gain_db[slot] = (int8_t)gain_db;
        if (app->mixer.initialized) {
            native_mixer_set_source_gain(&app->mixer, slot, native_ui_mixer_gain_db_to_q15(gain_db));
        }
        pthread_mutex_unlock(&app->video_lock);
    }
    native_mixer_overlay_touch(app);
}

static void native_mixer_overlay_adjust(App *app, int delta_db) {
    int slot = app->mixer_overlay_selected;
    native_mixer_overlay_set_db(app, slot, (int)app->mixer_gain_db[slot] + delta_db);
}

/* While the overlay is open its navigation keys must not leak to the RDP session — both
 * edges are swallowed (a release reaching the server without its press confuses nobody,
 * but a swallowed press with a leaked release would). Everything else (typing on the USB
 * keyboard) passes through untouched. */
static bool native_mixer_overlay_consumes_evdev_key(uint16_t code) {
    switch (code) {
    case 1:   /* KEY_ESC */
    case 28:  /* KEY_ENTER */
    case 96:  /* KEY_KPENTER */
    case 103: /* KEY_UP */
    case 105: /* KEY_LEFT */
    case 106: /* KEY_RIGHT */
    case 108: /* KEY_DOWN */
    case 158: /* KEY_BACK */
    case 352: /* KEY_OK */
        return true;
    default:
        return false;
    }
}

static void native_mixer_overlay_evdev_key(App *app, uint16_t code) {
    switch (code) {
    case 103: /* KEY_UP */
        native_mixer_overlay_adjust(app, NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case 108: /* KEY_DOWN */
        native_mixer_overlay_adjust(app, -NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case 105: /* KEY_LEFT */
        native_mixer_overlay_select(app, app->mixer_overlay_selected - 1);
        break;
    case 106: /* KEY_RIGHT */
        native_mixer_overlay_select(app, app->mixer_overlay_selected + 1);
        break;
    default: /* enter/ok/esc/back */
        native_mixer_overlay_hide(app);
        break;
    }
}

/* Remote navigation when the remote is NOT evdev-grabbed (the SDL fallback path, same
 * split as the color keys): arrows/enter arrive as ordinary SDL keys, Back as a webOS
 * scancode. */
static void native_mixer_overlay_sdl_key(App *app, const SDL_KeyboardEvent *event) {
#if HELLOLG_HAVE_SDL_WEBOS_CURSOR
    if (event->keysym.scancode == SDL_WEBOS_SCANCODE_BACK || event->keysym.scancode == 482) {
        native_mixer_overlay_hide(app);
        return;
    }
#else
    if (event->keysym.scancode == 482) {
        native_mixer_overlay_hide(app);
        return;
    }
#endif
    switch (event->keysym.sym) {
    case SDLK_UP:
        native_mixer_overlay_adjust(app, NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case SDLK_DOWN:
        native_mixer_overlay_adjust(app, -NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
        break;
    case SDLK_LEFT:
        native_mixer_overlay_select(app, app->mixer_overlay_selected - 1);
        break;
    case SDLK_RIGHT:
        native_mixer_overlay_select(app, app->mixer_overlay_selected + 1);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_ESCAPE:
        native_mixer_overlay_hide(app);
        break;
    default:
        break;
    }
}

/* Presents the overlay (both renderers live in ui_mixer.c). The LVGL path renders LIVE —
 * per-tick, the meters animate with playback — while the raw-SDL fallback stays
 * latch-drawn like the indicator. Cleared back to the punch frame on hide/expiry.
 * Skipped while the RemoteFX RGBA path owns the renderer (native_mixer_overlay_show
 * refuses to open there). */
static void native_present_mixer_overlay_frame(App *app, SDL_Renderer *renderer) {
    if (!app->mixer_overlay_visible) {
        return;
    }
    if (SDL_TICKS_PASSED(SDL_GetTicks(), app->mixer_overlay_hide_ticks)) {
        native_mixer_overlay_hide(app);
        return;
    }
    unsigned connected_mask = 0;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        if (slot->rdp && atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE) {
            connected_mask |= 1u << i;
        }
    }
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    NativeUiMixer *mixer_ui = native_preconnect_ui_mixer(app->preconnect_ui);
    if (mixer_ui) {
        /* Snapshot the meter peaks under video_lock: an rdp-worker may be destroying and
         * reinitializing app->mixer during a format re-pin (it does so under the same
         * lock), and the render path must never touch a mixer mid-teardown. */
        int32_t peaks[NATIVE_SETTINGS_MAX_SESSIONS][2] = {{0, 0}};
        pthread_mutex_lock(&app->video_lock);
        if (app->mixer.initialized) {
            for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
                native_mixer_get_source_peaks(&app->mixer, i, &peaks[i][0], &peaks[i][1]);
            }
        }
        pthread_mutex_unlock(&app->video_lock);
        native_ui_mixer_render(mixer_ui, peaks, app->mixer_gain_db, app->mixer_overlay_selected,
                               connected_mask, SDL_GetTicks());
        app->video_plane_punched = true; /* screen content is ours, not the punch frame */
        return;
    }
#endif
    if (app->mixer_overlay_drawn) {
        return;
    }
    native_ui_mixer_draw_fallback(renderer, app->mixer_gain_db, app->mixer_overlay_selected, connected_mask);
    app->mixer_overlay_drawn = true;
    app->video_plane_punched = true; /* screen content is ours, not the punch frame */
}

static void native_present_streaming_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (native_present_rgba_frame(app, renderer, logged) != 0) {
        return;
    }
    if (app->switch_deadline_ticks != 0) {
        /* A swap is in flight (keyframe watchdog armed): cover the reload window. */
        native_present_switch_splash(app, renderer);
        return;
    }
    if (app->switch_splash_drawn) {
        /* Swap finished: reveal the new stream (re-punch) and repaint the badge. */
        app->switch_splash_drawn = false;
        app->indicator_drawn = false;
        app->video_plane_punched = false;
    }
    if (app->mixer_overlay_visible) {
        native_present_mixer_overlay_frame(app, renderer);
        return;
    }
    if (app->indicator_slot >= 0) {
        native_present_indicator_frame(app, renderer);
        return;
    }
    native_present_renderer_frame(app, renderer, logged);
}

static void native_show_session_indicator(App *app, int slot) {
    app->indicator_slot = slot;
    app->indicator_until_ticks = SDL_GetTicks() + NATIVE_INDICATOR_SHOW_MS;
    app->indicator_drawn = false;
}

/* ---- Video switching between session slots (SDL thread only) ---- */

/* Finalizes a switch to `target`, which must already be connected. Retargets input and
 * cursor, drops the shared video path (the mixed audio track survives via
 * native_reopen_audio_locked), and asks the servers to pause/resume their graphics. */
static void native_complete_session_switch(App *app, int target) {
    int old_index = atomic_load(&app->active_index);
    NativeSessionSlot *target_slot = &app->sessions[target];
    if (!target_slot->rdp) {
        return;
    }
    if (old_index == target) {
        native_show_session_indicator(app, target);
        return;
    }
    NativeSessionSlot *old_slot = &app->sessions[old_index];
    fprintf(stderr, "[native] switching video %s -> %s\n", native_session_slot_name(old_index),
            native_session_slot_name(target));
    /* Reachable with the overlay up via the auto-switch on a died session. */
    native_mixer_overlay_hide(app);

    /* Releases still owed to the OLD server must go out before input is retargeted,
     * or its desktop keeps a stuck drag / auto-repeating key. */
    native_flush_held_inputs(app);
    native_input_set_active(&app->input, false);

    /* Route worker callbacks to the new slot; the old session's in-flight AUs stop
     * feeding the shared decoder immediately (active check in on_video_au). The video
     * track itself is NOT closed here: the old picture stays up and on_video_au swaps
     * the decoder in one pipeline reload once the new stream's keyframe is in hand —
     * no black gap, one audio interruption instead of two. */
    /* Snapshot the watchdog baseline BEFORE the target becomes active: its worker can
     * decode the resume keyframe at any point after the switch below, and on a static
     * desktop that keyframe is the only frame coming — baselining after it would make
     * the watchdog reconnect a successful switch and mislearn refresh_ineffective. */
    unsigned switch_baseline = atomic_load(&target_slot->video_ok_frames);
    atomic_store(&app->active_index, target);

    pthread_mutex_lock(&app->video_lock);
    if (app->rgba) {
        native_defer_rgba_texture_destroy(app);
        native_rgba_surface_close(app->rgba);
        app->rgba = NULL;
    }
    app->video_plane_punched = false;
    pthread_mutex_unlock(&app->video_lock);

    /* Input now drives the new session; the per-tick derive block re-arms the active
     * flag and re-syncs NumLock. */
    native_input_set_session(&app->input, target_slot->rdp);
    native_input_set_desktop_size(&app->input, (uint16_t)atomic_load(&target_slot->desktop_width),
                                  (uint16_t)atomic_load(&target_slot->desktop_height));
    app->input_locks_synced = false;
    native_request_pointer_window_size_update(app);
    native_cursor_reassert(&target_slot->cursor);

    /* Background the old server (graphics off, rdpsnd audio keeps feeding the mix) and
     * wake the new one. A previously suppressed session resumes with a delta frame the
     * reloaded hardware decoder cannot use, so also force a fresh keyframe; a session
     * that was never suppressed (fresh connect-on-demand) starts from its own IDR. */
    if (old_slot->rdp) {
        rdp_set_suppress_output(old_slot->rdp, false);
        old_slot->suppressed = true;
    }
#ifndef NATIVE_SWITCH_TEST_NO_RECONNECT
#define NATIVE_SWITCH_TEST_NO_RECONNECT 0
#endif
    if (target_slot->suppressed) {
        target_slot->suppressed = false;
        if (target_slot->refresh_ineffective && !NATIVE_SWITCH_TEST_NO_RECONNECT) {
            /* This server never delivers a keyframe on request (no Display Control
             * channel, Refresh Rect ignored — learned from an earlier watchdog timeout).
             * Skip the doomed wait and reconnect right away: a fresh connection always
             * starts with an IDR. */
            fprintf(stderr,
                    "[native] %s server yields no keyframe on request; reconnecting immediately for a fresh IDR\n",
                    native_session_slot_name(target));
            if (!native_slot_connect(app, target)) {
                /* Startup failure (e.g. worker spawn): route it through the standard
                 * failure drain — configurator with a status / auto-switch to a survivor
                 * — instead of leaving a dead slot behind a frozen stream. */
                slot_stop_with_state(target_slot, RDP_STATE_NETWORK_ERROR,
                                     rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
            }
        } else {
            rdp_set_suppress_output(target_slot->rdp, true);
            rdp_request_refresh(target_slot->rdp);
        }
    }

    /* Watchdog: if no decodable frame arrives, reconnect the slot (a fresh connection
     * always starts with an IDR — covers servers where neither suppress-resume nor the
     * display-control refresh produces a keyframe, e.g. grd mirror mode). */
    app->switch_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
    app->switch_baseline_frames = switch_baseline;
    app->switch_reconnect_used = false;

    native_show_session_indicator(app, target);
}

/* Navigates the screen to `target`'s configurator (pre-connect form) while any current
 * session keeps running in the background: its graphics are suppressed server-side and
 * its audio keeps feeding the mix. */
static void native_show_slot_configurator(App *app, int target) {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    int old_index = atomic_load(&app->active_index);
    NativeSessionSlot *old_slot = &app->sessions[old_index];
    if (old_index != target) {
        fprintf(stderr, "[native] showing the %s session configurator\n", native_session_slot_name(target));
        /* Releases owed to the old server must go out while input is still wired to it;
         * the evdev grab is dropped because the configurator needs the SDL mouse. */
        native_stop_streaming_input(app);
        native_input_set_active(&app->input, false);
        if (old_slot->rdp && !old_slot->suppressed) {
            /* Works for a still-CONNECTING slot too: the Rust worker buffers control
             * commands and replays them once the session goes active, so a slot left
             * mid-handshake still comes up backgrounded (suppressed) instead of
             * streaming video nobody displays. */
            rdp_set_suppress_output(old_slot->rdp, false);
            old_slot->suppressed = true;
        }
        atomic_store(&app->active_index, target);
        pthread_mutex_lock(&app->video_lock);
        if (app->rgba) {
            native_defer_rgba_texture_destroy(app);
            native_rgba_surface_close(app->rgba);
            app->rgba = NULL;
        }
        /* The video track stays open behind the (opaque) configurator: closing it would
         * reload the shared pipeline and cut the mixed audio for nothing. Returning to a
         * stream swaps the decoder on that stream's next keyframe (on_video_au). */
        app->video_plane_punched = false;
        pthread_mutex_unlock(&app->video_lock);
        native_input_set_session(&app->input, app->sessions[target].rdp);
        /* The configurator needs the plain SDL arrow, but the old session keeps running
         * backgrounded: leave its cached cursor shape/hidden state alone so switching
         * back can reassert it (the server resends nothing while suppressed). */
        native_cursor_show_default();
        app->switch_deadline_ticks = 0;
    }
    app->streaming_visible = false;
    app->ui_last_state = -1;
    if (app->preconnect_ui) {
        /* Any in-flight connect banner belongs to the slot we are leaving; its session
         * continues in the background and reports through per-slot status lines. */
        native_preconnect_ui_set_connecting(app->preconnect_ui, false, "");
        native_preconnect_ui_select_slot(app->preconnect_ui, target);
        native_preconnect_ui_set_visible(app->preconnect_ui, true);
    }
#else
    (void)app;
    (void)target;
    fprintf(stderr, "[native] no pre-connect UI in this build; cannot open a session configurator\n");
#endif
}

/* Color-button press: every button is "go to that session's screen" — the live stream
 * when the slot is connected, its configurator otherwise. */
static void native_request_session_switch(App *app, int target) {
    if (!app || target < 0 || target >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    if (app->mixer_overlay_visible) {
        /* Mixer mode: a color key selects that slot's slider; the ACTIVE slot's key (the
         * one that opened the overlay) toggles it back off. Switching resumes once the
         * overlay is gone. */
        if (target == atomic_load(&app->active_index)) {
            native_mixer_overlay_hide(app);
        } else {
            native_mixer_overlay_select(app, target);
        }
        return;
    }
    NativeSessionSlot *slot = &app->sessions[target];
    if (slot->rdp && atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE) {
        if (target == atomic_load(&app->active_index) && app->streaming_visible) {
            /* Re-pressing the active slot's button is no switch — open the live volume
             * mixer instead of just re-flashing the badge. */
            native_mixer_overlay_show(app);
            return;
        }
        native_complete_session_switch(app, target);
        return;
    }
    if (target == atomic_load(&app->active_index)) {
        return; /* already on this slot's configurator (or it is mid-connect on screen) */
    }
    native_show_slot_configurator(app, target);
}

/* Per-tick bookkeeping: worker-side refresh requests and the keyframe watchdog. */
static void native_switch_tick(App *app) {
    if (atomic_exchange(&app->video_refresh_needed, false)) {
        NativeSessionSlot *active = native_active_slot(app);
        if (active->rdp && atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE) {
            /* Baseline before the refresh request: the worker may decode the requested
             * keyframe before the next line runs (see complete_session_switch). */
            app->switch_baseline_frames = atomic_load(&active->video_ok_frames);
            rdp_request_refresh(active->rdp);
            app->switch_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
            app->switch_reconnect_used = false;
        }
    }

    if (app->switch_deadline_ticks != 0) {
        NativeSessionSlot *watched = native_active_slot(app);
        if (atomic_load(&watched->video_ok_frames) != app->switch_baseline_frames) {
            app->switch_deadline_ticks = 0; /* frames flowing; watchdog satisfied */
        } else if (watched->rdp && atomic_load(&watched->current_state) != (int)RDP_STATE_ACTIVE) {
            /* Still (re)connecting: the keyframe timeout must count from ACTIVE, not from
             * the button press — otherwise a slow TLS/CredSSP handshake makes the watchdog
             * fire mid-reconnect and thrash the server with a second reconnect. */
            app->switch_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
        } else if (SDL_TICKS_PASSED(SDL_GetTicks(), app->switch_deadline_ticks)) {
            NativeSessionSlot *active = native_active_slot(app);
            if (NATIVE_SWITCH_TEST_NO_RECONNECT) {
                fprintf(stderr,
                        "[native] TEST: watchdog expired; would reconnect the %s session, keeping the connection\n",
                        native_session_slot_name(active->index));
                app->switch_deadline_ticks = SDL_GetTicks() + 10000u; /* re-check, keep logging */
            } else if (!app->switch_reconnect_used && active->rdp &&
                atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE) {
                fprintf(stderr,
                        "[native] no keyframe %ums after the switch; reconnecting the %s session for a fresh IDR\n",
                        (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS, native_session_slot_name(active->index));
                /* Remember: switches to this slot should reconnect immediately from now on. */
                active->refresh_ineffective = true;
                app->switch_reconnect_used = true;
                app->switch_deadline_ticks = SDL_GetTicks() + 2u * NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
                app->switch_baseline_frames = atomic_load(&active->video_ok_frames);
                if (!native_slot_connect(app, active->index)) {
                    /* Same failure routing as the fast-reconnect path above. */
                    slot_stop_with_state(active, RDP_STATE_NETWORK_ERROR,
                                         rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
                }
            } else {
                fprintf(stderr, "[native] switch watchdog gave up waiting for video frames\n");
                app->switch_deadline_ticks = 0;
            }
        }
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
        native_stop_streaming_input(app);
        break;
    case SDL_APP_DIDENTERBACKGROUND:
        fprintf(stderr, "[native] SDL_APP_DIDENTERBACKGROUND; keeping native session running\n");
        native_stop_streaming_input(app); /* belt and suspenders */
        break;
    case SDL_APP_WILLENTERFOREGROUND:
        fprintf(stderr, "[native] SDL_APP_WILLENTERFOREGROUND\n");
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        fprintf(stderr, "[native] SDL_APP_DIDENTERFOREGROUND\n");
        app->window_unfocused = false;
        native_resume_streaming_input(app, window);
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
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST || event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                   event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            /* A remote-invoked webOS overlay (TV menu, notifications) steals input focus WITHOUT
             * fully backgrounding the app, so SDL_APP_WILLENTERBACKGROUND never fires. Release
             * the global evdev grab here too, otherwise the mouse/keyboard stay locked to us and
             * are unusable in the overlay. Re-grab happens on FOCUS_GAINED below. */
            fprintf(stderr, "[native] window lost focus (event %u); releasing input grab\n",
                    (unsigned)event->window.event);
            app->window_unfocused = true;
            native_stop_streaming_input(app);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event->window.event == SDL_WINDOWEVENT_RESTORED ||
                   event->window.event == SDL_WINDOWEVENT_SHOWN) {
            fprintf(stderr, "[native] window gained focus (event %u); re-grabbing input\n",
                    (unsigned)event->window.event);
            app->window_unfocused = false;
            native_resume_streaming_input(app, window);
        } else if (event->window.event == SDL_WINDOWEVENT_EXPOSED) {
            fprintf(stderr, "[native] SDL window lifecycle event %u\n", (unsigned)event->window.event);
        }
        break;
    case SDL_MOUSEMOTION:
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
        if (app->mixer_overlay_visible) {
            /* The pointer belongs to the overlay: the evdev grab is released while it is
             * up, so the system cursor is live and these are compositor coordinates. */
            if (app->mixer_overlay_dragging) {
                int win_w = 0;
                int win_h = 0;
                SDL_GetWindowSize(window, &win_w, &win_h);
                native_mixer_overlay_set_db(app, app->mixer_overlay_selected,
                                            native_ui_mixer_fader_db_at(win_h, event->motion.y));
            }
            break;
        }
#endif
        if (app->window_unfocused) {
            break; /* an overlay/menu has focus: the released mouse must not move the RDP cursor */
        }
#if HELLOLG_WITH_EVDEV_INPUT
        if (native_evdev_input_mouse_active(&app->evdev_input)) {
            break; /* a grabbed USB mouse is read via evdev */
        }
#endif
        /* Fallback when no USB mouse was grabbed: SDL delivers the compositor's own pointer
         * (Magic Remote, trackpad remote) in absolute window coordinates. */
        if (app->cursor_reassert_pending) {
            native_cursor_reassert(&native_active_slot(app)->cursor);
            app->cursor_reassert_pending = false;
        }
        native_set_virtual_mouse_position(app, event->motion.x, event->motion.y);
        native_input_pointer_move(&app->input, event->motion.x, event->motion.y);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
        if (app->mixer_overlay_visible) {
            if (event->type == SDL_MOUSEBUTTONUP) {
                app->mixer_overlay_dragging = false;
                native_mixer_overlay_touch(app);
                break;
            }
            if (event->button.button != SDL_BUTTON_LEFT) {
                native_mixer_overlay_touch(app);
                break;
            }
            int win_w = 0;
            int win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            int hit_slot = -1;
            bool on_fader = false;
            if (!native_ui_mixer_hit_test(win_w, win_h, event->button.x, event->button.y, &hit_slot, &on_fader)) {
                native_mixer_overlay_hide(app); /* click outside the panel closes it */
                break;
            }
            if (hit_slot >= 0) {
                native_mixer_overlay_select(app, hit_slot);
                if (on_fader) {
                    native_mixer_overlay_set_db(app, hit_slot,
                                                native_ui_mixer_fader_db_at(win_h, event->button.y));
                    app->mixer_overlay_dragging = true;
                }
            } else {
                native_mixer_overlay_touch(app);
            }
            break;
        }
#endif
        if (app->window_unfocused) {
            break;
        }
#if HELLOLG_WITH_EVDEV_INPUT
        if (native_evdev_input_mouse_active(&app->evdev_input)) {
            break;
        }
#endif
        NativeInputButton button = native_button_from_sdl(event->button.button);
        if (button != 0) {
            native_set_virtual_mouse_position(app, event->button.x, event->button.y);
            native_input_pointer_button(&app->input, atomic_load(&app->virtual_mouse_x),
                                        atomic_load(&app->virtual_mouse_y), button,
                                        event->type == SDL_MOUSEBUTTONDOWN);
            native_track_button(app, button, event->type == SDL_MOUSEBUTTONDOWN);
        }
        break;
    }
    case SDL_MOUSEWHEEL: {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
        if (app->mixer_overlay_visible) {
            int overlay_wheel_y = event->wheel.y;
#if SDL_VERSION_ATLEAST(2, 0, 4)
            if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                overlay_wheel_y = -overlay_wheel_y;
            }
#endif
            if (overlay_wheel_y != 0) {
                native_mixer_overlay_adjust(app, overlay_wheel_y * NATIVE_MIXER_OVERLAY_GAIN_STEP_DB);
            }
            break;
        }
#endif
        if (app->window_unfocused) {
            break;
        }
#if HELLOLG_WITH_EVDEV_INPUT
        if (native_evdev_input_mouse_active(&app->evdev_input)) {
            break;
        }
#endif
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
    case SDL_KEYUP:
        /* The USB keyboard is read from grabbed /dev/input (input_evdev); there is no SDL
         * keyboard path. The only keys taken from SDL are the webOS remote's four color
         * keys (session-slot navigation): they come from the ungrabbed TV remote, not the
         * grabbed keyboard. App exit is system-driven (webOS EXIT/home -> SDL_QUIT /
         * SDL_APP_TERMINATING above).
         *
         * The mouse, by contrast, keeps an SDL fallback above: a grabbed USB mouse is read via
         * evdev, but with no USB mouse SDL still delivers the compositor pointer (Magic Remote). */
        if (event->type == SDL_KEYDOWN) {
            int slot = native_sdl_webos_color_slot(&event->key);
            if (slot >= 0) {
                native_request_session_switch(app, slot);
            } else if (app->mixer_overlay_visible) {
                native_mixer_overlay_sdl_key(app, &event->key);
            }
        }
        break;
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

    SDL_FilterEvents(native_filter_webos_system_keys, app);

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

static int native_run_app_loop(App *app, NativeSettings *settings) {
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
    /* Cursor visibility is event-driven from here on: the preconnect UI keeps the default
     * arrow, and during a session the server's pointer updates drive shape/visibility
     * through native_cursor_apply (cursor_sdl.c). */

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

    uint16_t loop_fps = settings->sessions[NATIVE_SESSION_SLOT_GREEN].fps;
    fprintf(stderr, "[native] SDL loop running at target %u fps\n", (unsigned)loop_fps);

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    NativePreconnectUi *ui =
        native_preconnect_ui_create(window, renderer, settings->sessions, settings->audio_prebuffer_ms,
                                    settings->audio_codec);
    if (!ui) {
        fprintf(stderr, "[native-ui] failed to create pre-connect UI\n");
        SDL_StopTextInput();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
    app->preconnect_ui = ui;

    app->interactive_ui = true;
    app->ui_last_state = -1;
    app->streaming_visible = false;
    bool present_logged = false;

#if HELLOLG_WITH_EVDEV_INPUT
    /* The keyboard is read from grabbed /dev/input with no SDL fallback; warn on the visible
     * preconnect screen if none is attached so it isn't discovered only after connecting. */
    if (!native_evdev_input_probe_keyboard()) {
        native_preconnect_ui_set_status(ui, "No USB keyboard detected — you can connect, but keyboard input needs one.",
                                        true);
    }
#endif

    while (atomic_load(&app->running)) {
        NativeSessionSlot *active = native_active_slot(app);
        int event_state = atomic_load(&active->current_state);
        if (active->rdp && event_state == (int)RDP_STATE_ACTIVE) {
            native_drain_pointer_clamp(app);
            native_drain_pointer_warp(app, window);
            native_cursor_tick(app);
#if HELLOLG_WITH_EVDEV_INPUT
            native_drain_evdev_mouse(app, window);
            native_drain_evdev_keyboard(app);
#endif
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handle_sdl_event(app, window, renderer, &event);
            }
        } else {
            native_pump_preconnect_system_events(app, window, renderer, ui, &window_width, &window_height);
        }

        native_switch_tick(app);

        /* Drain per-slot terminal events. The active slot decides between an automatic
         * switch to the surviving session and a return to the pre-connect UI; a
         * background slot is torn down quietly (the mix simply loses its audio). */
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            NativeSessionSlot *slot = &app->sessions[i];
            if (!atomic_load(&slot->session_failed)) {
                continue;
            }
            RdpState terminal_state = (RdpState)atomic_load(&slot->terminal_state);
            char status[128];
            if (terminal_state == RDP_STATE_STOPPED) {
                (void)snprintf(status, sizeof(status), "%s session stopped.", native_session_slot_name(i));
            } else {
                (void)snprintf(status, sizeof(status), "%s session failed: %s", native_session_slot_name(i),
                               rdp_state_name(terminal_state));
            }

            if (i == atomic_load(&app->active_index)) {
                int survivor = -1;
                for (int j = 0; j < NATIVE_SETTINGS_MAX_SESSIONS; j++) {
                    if (j != i && app->sessions[j].rdp && !atomic_load(&app->sessions[j].session_failed) &&
                        atomic_load(&app->sessions[j].current_state) == (int)RDP_STATE_ACTIVE) {
                        survivor = j;
                    }
                }
                if (survivor >= 0 && app->streaming_visible) {
                    /* The session the user was WATCHING died: fall back to the survivor. */
                    fprintf(stderr, "[native] %s; auto-switching video to the %s session\n", status,
                            native_session_slot_name(survivor));
                    native_complete_session_switch(app, survivor);
                    native_stop_slot(app, i);
                    native_preconnect_ui_set_status(ui, status, true);
                } else if (survivor >= 0) {
                    /* A connect attempt made from this slot's configurator failed while
                     * another session runs backgrounded: stay on the form so the user can
                     * read the error and fix the values, instead of bouncing to the
                     * survivor's stream on every retry. */
                    native_stop_slot(app, i);
                    app->ui_last_state = -1;
                    native_preconnect_ui_set_connecting(ui, false, status);
                    native_preconnect_ui_set_status(ui, status, true);
                } else {
                    /* Flush + release input while the failed session pointer is still
                     * wired, then tear the slot down (media follows after the pass once
                     * no slot needs it). */
                    native_stop_streaming_input(app);
                    native_stop_slot(app, i);
                    native_cursor_reset(&slot->cursor);
                    app->switch_deadline_ticks = 0;
                    native_mixer_overlay_hide(app); /* streaming screen is going away */
                    app->streaming_visible = false;
                    app->ui_last_state = -1;
                    native_preconnect_ui_set_visible(ui, true);
                    native_preconnect_ui_set_connecting(ui, false, status);
                    native_preconnect_ui_set_status(ui, status, true);
                }
            } else {
                fprintf(stderr, "[native] background %s\n", status);
                native_stop_slot(app, i);
                native_preconnect_ui_set_status(ui, status, true);
            }
            /* After each drained failure: when NO slot holds a session anymore, the
             * shared media pipeline must go too. Checked per drained slot (not inside
             * the branches) so an active+background double failure in one pass — where
             * the active slot drains first while the background one still counts as
             * connected — cannot leak the pipeline. */
            if (!native_any_slot_connected(app)) {
                native_stop_media(app);
            }
        }

        active = native_active_slot(app);
        int state = atomic_load(&active->current_state);
        /* Input arming is derived here, on the SDL thread, from the active slot's state:
         * worker callbacks must not write shared input state (see on_state). */
        /* The mixer overlay owns the pointer while visible (evdev grab released, system
         * cursor shown): keep RDP input disarmed so nothing leaks to the session. */
        bool input_streaming = active->rdp && state == (int)RDP_STATE_ACTIVE && !app->mixer_overlay_visible;
        native_input_set_active(&app->input, input_streaming);
        /* Input coordinate mapping follows the ACTIVE slot's desktop size, derived here
         * like the arming flag: workers only publish their own slot's atomics (a worker
         * writing app->input directly would race a concurrent session switch). */
        uint16_t active_desk_w = (uint16_t)atomic_load(&active->desktop_width);
        uint16_t active_desk_h = (uint16_t)atomic_load(&active->desktop_height);
        if (active->rdp && active_desk_w != 0 && active_desk_h != 0 &&
            (active_desk_w != (uint16_t)atomic_load(&app->input.desktop_width) ||
             active_desk_h != (uint16_t)atomic_load(&app->input.desktop_height))) {
            native_input_set_desktop_size(&app->input, active_desk_w, active_desk_h);
            native_request_pointer_window_size_update(app);
        }
        if (input_streaming && !app->input_locks_synced) {
            /* Fresh sessions start with the server's toggle keys in an unknown state;
             * force NumLock on so an attached keyboard's numpad types digits instead of
             * navigating. The TV has no lock-state source of its own to mirror (webOS SDL
             * does not track keyboard LEDs), and a NumLock key press still toggles the
             * server normally. */
            native_input_sync_locks(&app->input, false, true, false);
            app->input_locks_synced = true;
        } else if (!input_streaming) {
            app->input_locks_synced = false;
        }
        if (active->rdp && state != app->ui_last_state && state != (int)RDP_STATE_ACTIVE) {
            char status[64];
            (void)snprintf(status, sizeof(status), "%s...", rdp_state_name((RdpState)state));
            native_preconnect_ui_set_status(ui, status, false);
            app->ui_last_state = state;
        }
        if (active->rdp && state == (int)RDP_STATE_ACTIVE && !app->streaming_visible) {
            if (active->suppressed) {
                /* The user left this slot mid-connect (queueing a suppress in its worker)
                 * and returned before it went active, so no switch path ran to resume it:
                 * without this the buffered suppress lands right after activation and the
                 * on-screen session streams no video. Resume + force a keyframe exactly
                 * like a switch-back, and arm the watchdog for servers where the refresh
                 * request is a no-op (it reconnects for a fresh IDR). */
                active->suppressed = false;
                /* Baseline before resume/refresh: the keyframe may decode immediately. */
                app->switch_baseline_frames = atomic_load(&active->video_ok_frames);
                rdp_set_suppress_output(active->rdp, true);
                rdp_request_refresh(active->rdp);
                app->switch_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
                app->switch_reconnect_used = false;
            }
            (void)native_config_save_persisted(settings);
            native_preconnect_ui_set_visible(ui, false);
            app->window_unfocused = false; /* enter streaming focused, whatever fired during preconnect */
            /* Grab input now that streaming has started; the preconnect UI needs the SDL mouse,
             * so we don't grab earlier. Note any degradation on the (now hidden) preconnect
             * status so it shows if the session returns here; the loud log covers diagnosis. */
            switch (native_start_streaming_input(app)) {
            case NATIVE_INPUT_START_OK:
                break;
            case NATIVE_INPUT_START_NO_KEYBOARD:
                native_preconnect_ui_set_status(ui, "No USB keyboard detected — this session has no keyboard input.",
                                                true);
                break;
            case NATIVE_INPUT_START_UNAVAILABLE:
                native_preconnect_ui_set_status(
                    ui, "Input capture failed to start — this session has no mouse or keyboard input.", true);
                break;
            }
            app->streaming_visible = true;
            native_show_session_indicator(app, atomic_load(&app->active_index));
        }

        if (app->streaming_visible) {
            native_present_streaming_frame(app, renderer, &present_logged);
        }

        int requested_slot = NATIVE_SESSION_SLOT_GREEN;
        char requested_host[NATIVE_CONFIG_STRING_MAX];
        char requested_username[NATIVE_CONFIG_STRING_MAX];
        char requested_password[NATIVE_CONFIG_STRING_MAX];
        char requested_domain[NATIVE_CONFIG_STRING_MAX];
        uint16_t requested_port = 0;
        uint16_t requested_fps = 0;
        uint16_t requested_audio_prebuffer_ms = 0;
        uint16_t requested_audio_codec = NATIVE_AUDIO_CODEC_AUTO;
        /* ALWAYS consume the one-shot request: gating the take on slot state would leave
         * a Connect clicked on a still-connecting slot's configurator latched in the UI,
         * to be replayed as a surprise reconnect whenever that session later fails. */
        if (native_preconnect_ui_take_connect(ui, &requested_slot, requested_host, sizeof(requested_host),
                                              &requested_port, requested_username, sizeof(requested_username),
                                              requested_password, sizeof(requested_password), requested_domain,
                                              sizeof(requested_domain), &requested_fps,
                                              &requested_audio_prebuffer_ms, &requested_audio_codec)) {
            if (requested_slot < 0 || requested_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
                requested_slot = NATIVE_SESSION_SLOT_GREEN;
            }
            NativeSessionConfig *session = &settings->sessions[requested_slot];
            NativeSessionSlot *requested_live = &app->sessions[requested_slot];
            if (requested_live->rdp &&
                atomic_load(&requested_live->current_state) != (int)RDP_STATE_ACTIVE) {
                /* Mid-handshake: joining the worker here could block the SDL thread for
                 * the whole connect timeout, so reject instead (an ACTIVE background
                 * session stays reconnectable through the branch below). */
                char busy_status[96];
                (void)snprintf(busy_status, sizeof(busy_status), "The %s session is still connecting.",
                               native_session_slot_name(requested_slot));
                native_preconnect_ui_set_status(ui, busy_status, true);
            } else if (!copy_config_string(session->host, sizeof(session->host), requested_host, "host")) {
                native_preconnect_ui_set_status(ui, "Host value is too long.", true);
            } else if (!copy_config_string(session->username, sizeof(session->username), requested_username,
                                           "username")) {
                native_preconnect_ui_set_status(ui, "Username value is too long.", true);
            } else if (!copy_config_string(session->password, sizeof(session->password), requested_password,
                                           "password")) {
                native_preconnect_ui_set_status(ui, "Password value is too long.", true);
            } else if (!copy_config_string(session->domain, sizeof(session->domain), requested_domain, "domain")) {
                native_preconnect_ui_set_status(ui, "Domain value is too long.", true);
            } else {
                session->port = requested_port;
                session->fps = requested_fps;
                settings->audio_prebuffer_ms = requested_audio_prebuffer_ms;
                settings->audio_codec = requested_audio_codec;
                /* Applies to sessions started from now on; live sessions keep their
                 * negotiated stream until they reconnect. Sample rates may differ across
                 * codecs (Opus 48k vs grd PCM 44.1k), so a mid-flight change can leave a
                 * new session muted until the others reconnect too. */
                if (app->audio_codec != requested_audio_codec) {
                    fprintf(stderr, "[native-audio] audio codec preference now %s (new connections only)\n",
                            requested_audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto");
                    app->audio_codec = requested_audio_codec;
                }
                /* mixer init state and the prebuffer are video_lock-guarded (workers run
                 * native_ensure_mixer_locked concurrently). */
                pthread_mutex_lock(&app->video_lock);
                if (!app->mixer.initialized) {
                    app->audio_prebuffer_ms = requested_audio_prebuffer_ms;
                } else if (app->audio_prebuffer_ms != requested_audio_prebuffer_ms) {
                    fprintf(stderr,
                            "[native-audio] audio buffer change to %ums applies after an app restart (mixer already running at %ums)\n",
                            (unsigned)requested_audio_prebuffer_ms, (unsigned)app->audio_prebuffer_ms);
                }
                pthread_mutex_unlock(&app->video_lock);
                native_config_apply_initial_desktop_hint(settings);
                char validation_message[128];
                if (!native_session_config_validate_connect(session, requested_slot, validation_message,
                                                            sizeof(validation_message))) {
                    native_preconnect_ui_set_status(ui, validation_message, true);
                } else {
                    /* The user may have edited OTHER slots' forms without connecting
                     * them; pull those values into the settings too, or the save on
                     * ACTIVE would persist stale data and drop the edits on restart.
                     * Live sessions keep their own slot->config until they reconnect. */
                    for (int other = 0; other < NATIVE_SETTINGS_MAX_SESSIONS; other++) {
                        if (other != requested_slot) {
                            (void)native_preconnect_ui_get_slot_values(ui, other, &settings->sessions[other]);
                        }
                    }
                    native_config_log_effective(settings);
                    native_preconnect_ui_set_connecting(ui, true, "Connecting...");
                    /* Connect can target a slot whose session still runs in the
                     * background (form selected without navigating). Join that worker
                     * BEFORE overwriting slot->config — workers read it lock-free for
                     * log redaction — and reconnect it with the new values. */
                    native_stop_slot(app, requested_slot);
                    if (strcmp(app->sessions[requested_slot].config.host, session->host) != 0 ||
                        app->sessions[requested_slot].config.port != session->port) {
                        /* A different server now occupies this color slot: the learned
                         * no-keyframe-on-request workaround belonged to the old one. */
                        app->sessions[requested_slot].refresh_ineffective = false;
                    }
                    pthread_mutex_lock(&app->redaction_lock);
                    app->sessions[requested_slot].config = *session;
                    pthread_mutex_unlock(&app->redaction_lock);
                    atomic_store(&app->active_index, requested_slot);
                    app->ui_last_state = -1;
                    if (!native_slot_connect(app, requested_slot)) {
                        native_preconnect_ui_set_connecting(ui, false, "Connection failed: start failed");
                        native_preconnect_ui_set_status(ui, "Connection failed: start failed", true);
                    } else {
                        native_update_pointer_window_size(app);
                    }
                }
            }
        }

        native_preconnect_ui_tick(ui);
        uint16_t active_fps = native_active_slot(app)->config.fps;
        if (active_fps == 0) {
            active_fps = loop_fps;
        }
        native_wait_for_loop_tick(app, active_fps > 0 ? (uint32_t)(1000u / active_fps) : 16u);
    }

    app->preconnect_ui = NULL;
    native_preconnect_ui_destroy(ui);
#else
    const uint32_t delay_ms = loop_fps > 0 ? (uint32_t)(1000u / loop_fps) : 16u;
    bool present_logged = false;
    app->sessions[NATIVE_SESSION_SLOT_GREEN].config = settings->sessions[NATIVE_SESSION_SLOT_GREEN];
    if (!native_slot_connect(app, NATIVE_SESSION_SLOT_GREEN)) {
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
    (void)native_start_streaming_input(app);
    /* No pre-connect screen in this build: the stream IS the screen from the first tick.
     * The flag gates the active-color mixer-overlay toggle in
     * native_request_session_switch; without it the raw fallback panel is unreachable. */
    app->streaming_visible = true;

    while (atomic_load(&app->running)) {
        native_drain_pointer_clamp(app);
        native_drain_pointer_warp(app, window);
        native_cursor_tick(app);
#if HELLOLG_WITH_EVDEV_INPUT
        native_drain_evdev_mouse(app, window);
        native_drain_evdev_keyboard(app);
#endif
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle_sdl_event(app, window, renderer, &event);
        }
        native_switch_tick(app);

        NativeSessionSlot *arm_slot = native_active_slot(app);
        /* Same overlay gate as the preconnect loop: while the raw fallback panel is up,
         * input not consumed for its navigation must not leak to the session behind it. */
        bool input_streaming = arm_slot->rdp &&
                               atomic_load(&arm_slot->current_state) == (int)RDP_STATE_ACTIVE &&
                               !app->mixer_overlay_visible;
        native_input_set_active(&app->input, input_streaming);
        /* Same per-tick desktop-size derive as the preconnect-UI loop (see on_desktop_size). */
        uint16_t arm_desk_w = (uint16_t)atomic_load(&arm_slot->desktop_width);
        uint16_t arm_desk_h = (uint16_t)atomic_load(&arm_slot->desktop_height);
        if (arm_slot->rdp && arm_desk_w != 0 && arm_desk_h != 0 &&
            (arm_desk_w != (uint16_t)atomic_load(&app->input.desktop_width) ||
             arm_desk_h != (uint16_t)atomic_load(&app->input.desktop_height))) {
            native_input_set_desktop_size(&app->input, arm_desk_w, arm_desk_h);
            native_request_pointer_window_size_update(app);
        }
        if (input_streaming && !app->input_locks_synced) {
            native_input_sync_locks(&app->input, false, true, false);
            app->input_locks_synced = true;
        } else if (!input_streaming) {
            app->input_locks_synced = false;
        }

        if (renderer) {
            native_present_streaming_frame(app, renderer, &present_logged);
        } else {
            native_present_surface_frame(window, &present_logged);
        }
        native_wait_for_loop_tick(app, delay_ms);
    }
#endif

    /* Free the SDL cursor objects while the video subsystem is still up. */
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_cursor_reset(&app->sessions[i].cursor);
    }

    SDL_StopTextInput();
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    return atomic_load(&app->exit_code);
}
#else
static int native_run_app_loop(App *app, NativeSettings *settings) {
    (void)settings;
    fprintf(stderr,
            "[native] SDL event loop is not compiled in; deterministic smoke loop exits after start "
            "(enable HELLOLG_WITH_SDL for webOS lifecycle/input)\n");
    return atomic_load(&app->exit_code);
}
#endif

int main(int argc, char **argv) {
    native_prepare_webos_logging();
    native_prepare_webos_environment();

    NativeSettings native_settings;
    native_settings_defaults(&native_settings);

    const char *config_path = arg_value(argc, argv, "--config", NATIVE_CONFIG_PATH);
    bool config_required = arg_exists(argc, argv, "--config");
    if (!native_config_load_file(&native_settings, config_path, config_required) ||
        !native_config_apply_launch_params(&native_settings, argc, argv) ||
        !native_config_apply_cli(&native_settings, argc, argv)) {
        return 2;
    }
    native_config_apply_initial_desktop_hint(&native_settings);

    int sdl_result = native_prepare_sdl_runtime();
    if (sdl_result != 0) {
        return sdl_result;
    }

    bool ignore_saved_config = native_config_launch_ignores_saved_config(argc, argv);
    if (!native_config_load_persisted(&native_settings, ignore_saved_config) ||
        (config_required && !native_config_load_file(&native_settings, config_path, config_required)) ||
        !native_config_apply_launch_params(&native_settings, argc, argv) ||
        !native_config_apply_cli(&native_settings, argc, argv)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
    native_config_apply_initial_desktop_hint(&native_settings);

    native_config_log_effective(&native_settings);
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    if (!native_config_validate_runtime(&native_settings)) {
        native_shutdown_sdl_runtime();
        return 2;
    }
#else
    if (!native_config_validate(&native_settings)) {
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
    if (pthread_mutex_init(&app.redaction_lock, NULL) != 0) {
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return 2;
    }
    if (pthread_mutex_init(&app.mixer_repin_lock, NULL) != 0) {
        pthread_mutex_destroy(&app.redaction_lock);
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return 2;
    }
    atomic_init(&app.active_index, NATIVE_SESSION_SLOT_GREEN);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app.sessions[i];
        slot->app = &app;
        slot->index = i;
        slot->config = native_settings.sessions[i];
        native_cursor_init(&slot->cursor);
        atomic_init(&slot->current_state, (int)RDP_STATE_IDLE);
        atomic_init(&slot->terminal_state, (int)RDP_STATE_IDLE);
        atomic_init(&slot->session_failed, false);
        atomic_init(&slot->desktop_width, native_settings.width);
        atomic_init(&slot->desktop_height, native_settings.height);
        atomic_init(&slot->video_ok_frames, 0);
        atomic_init(&slot->connect_epoch, 0u);
        atomic_init(&slot->keyframe_wait_drops, 0u);
        app.mixer_gain_db[i] = 0; /* unity */
    }
    atomic_init(&app.video_refresh_needed, false);
    app.audio_prebuffer_ms = native_settings.audio_prebuffer_ms;
    app.audio_codec = native_settings.audio_codec;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    app.indicator_slot = -1;
    app.wheel_step = native_settings.wheel_step;
    app.wheel_scroll_divisor = native_settings.wheel_scroll_divisor;
    atomic_init(&app.render_width, 0);
    atomic_init(&app.render_height, 0);
#endif
    atomic_init(&app.running, true);
    atomic_init(&app.exit_code, 0);
    native_input_init(&app.input, NULL, native_settings.width, native_settings.height);

#if !defined(HELLOLG_WITH_SDL) || !HELLOLG_WITH_SDL
    app.sessions[NATIVE_SESSION_SLOT_GREEN].config = native_settings.sessions[NATIVE_SESSION_SLOT_GREEN];
    if (!native_slot_connect(&app, NATIVE_SESSION_SLOT_GREEN)) {
        int exit_code = atomic_load(&app.exit_code);
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            native_cursor_destroy(&app.sessions[i].cursor);
        }
        pthread_mutex_destroy(&app.mixer_repin_lock);
        pthread_mutex_destroy(&app.redaction_lock);
        pthread_mutex_destroy(&app.video_lock);
        native_shutdown_sdl_runtime();
        return exit_code ? exit_code : 2;
    }
#endif

    int loop_result = native_run_app_loop(&app, &native_settings);
    if (loop_result != 0 && atomic_load(&app.exit_code) == 0) {
        atomic_store(&app.exit_code, loop_result);
    }

    native_stop_all_sessions(&app);
    /* The pump feeds through video_lock; both are free here, so a join cannot deadlock. */
    native_mixer_destroy(&app.mixer);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_cursor_destroy(&app.sessions[i].cursor);
    }
    pthread_mutex_destroy(&app.mixer_repin_lock);
    pthread_mutex_destroy(&app.redaction_lock);
    pthread_mutex_destroy(&app.video_lock);
    native_shutdown_sdl_runtime();

    int exit_code = atomic_load(&app.exit_code);
    if (exit_code == 0 && app.decoder_errors > 0) {
        exit_code = rdp_state_exit_code(RDP_STATE_DECODER_ERROR);
    }
    return exit_code;
}
