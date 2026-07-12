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
#include <time.h>
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
#include "rdp_log.h"
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
#include "ui_preconnect.h"
#else
typedef void NativePreconnectUi;
#endif
#include "au_snapshot.h"
#include "audio_pipeline.h"
#include "audio_opus.h"
#include "luna_volume.h"
#include "ui_mixer.h"
#include "ui_slot_palette.h"
#include "audio_backend.h"
#include "media_backend.h"
#include "settings_json.h"
#include "video_rgba_sdl.h"
#include "video_backend.h"
#include "clog.h"

clog_define(g_native_log_config, cLogLevelInfo, cLogFlags_Default, "native", NULL);

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

/* If the freshly switched-to session produces no decodable frame within this window,
 * force a reconnect (guaranteed IDR) — covers servers where neither suppress-resume nor
 * the display-control refresh yields a keyframe (e.g. grd mirror mode). */
#define NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS 2000u
/* How long the colored active-session indicator stays on screen after a switch. */
#define NATIVE_INDICATOR_SHOW_MS 1500u
/* A snapshotted background stream must have been silent this long before its cache may
 * be replayed: covers the worker-queue + RTT tail after suppress (tens of ms) with a
 * wide margin, and rejects servers that ignore TS_SUPPRESS_OUTPUT_PDU outright. */
#define NATIVE_SNAPSHOT_QUIET_MS 500u
/* Hard ceiling on how long a deferred switch may keep the user on the old stream. The
 * normal worst case (2s keyframe deadline + reconnect + quiet window) fits well inside;
 * on expiry the press is answered with the immediate old-style switch — covers targets
 * that never quiet down (suppress ignored) or never produce an AU at all. */
#define NATIVE_PENDING_SWITCH_TIMEOUT_MS 8000u
/* Volume-mixer overlay: the fader model constants (range, step, auto-hide) and both
 * renderers live in ui_mixer.h/.c; this file keeps the state machine and key routing. */
/* System-volume poll cadence while streaming: the LS2 getVolume round-trip is an
 * in-process bus call (no subprocess), cheap enough to keep the MASTER fader and the
 * auto-raise detector live at 5 Hz. */
#define NATIVE_SYSTEM_VOLUME_POLL_MS 200u
/* System-volume poll cadence while the overlay is up (~2 Hz keeps the MASTER knob in
 * step with the remote's VOL keys at negligible bus traffic). */
#define NATIVE_MIXER_OVERLAY_VOLUME_POLL_MS 500u

_Static_assert(NATIVE_AUDIO_PIPELINE_MAX_SOURCES >= NATIVE_SETTINGS_MAX_SESSIONS,
               "every session slot needs an audio source");

typedef struct App App;

/* One remote-button session slot (green/yellow). The slot owns its RDP session handle,
 * connection config (stable while connected — on_log redaction and reconnects read it),
 * per-session server state (desktop size, cursor) and its audio-pipeline routing. The slot
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
    /* Audio-pipeline routing (worker thread of this session only). The negotiated
     * format snapshot is atomic because the SDL thread mirrors it into HUB metadata. */
    bool audio_routed;
    bool audio_incompatible_logged;
    atomic_uint audio_codec;
    atomic_uint audio_sample_rate;
    atomic_uint audio_channels;
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
    /* Compressed-AU snapshot of this slot's latest background (re)connect: the connect
     * IDR plus any deltas that raced the suppress request. Guarded by app->video_lock
     * (this slot's worker appends while backgrounded; the SDL thread arms, replays and
     * resets). Replaying it on switch-to rebuilds exactly the reference state the
     * server's next delta assumes — see au_snapshot.h for the protocol story. */
    NativeAuSnapshot snapshot;
    /* Worker -> SDL thread: the snapshot is seeded with a cached keyframe. */
    atomic_bool snapshot_idr_ready;
    /* SDL thread only: hidden background reconnect in flight for this slot; cleared when
     * the switch tick suppresses it (IDR cached) or a switch makes it active again. */
    bool snapshot_pending;
    /* SDL thread only: deadline for the pending snapshot to receive its first AU, armed
     * by the switch tick once the reconnect reaches ACTIVE (0 = not armed yet). A stream
     * that never feeds on_video_au — e.g. the RemoteFX bitmap path — would otherwise
     * stay pending and unsuppressed forever. */
    uint32_t snapshot_deadline_ticks;
    /* Monotonic-ms stamp of the latest background AU while the snapshot was armed
     * (worker writes, SDL thread reads). A stream still audible shortly before a replay
     * is either the (bounded) suppress tail or a server ignoring the suppress — either
     * way its in-flight AUs could interleave with the replay and corrupt the reference
     * chain, so the replay defers to the reconnect fallback until things go quiet. */
    atomic_uint snapshot_last_au_ms;
    /* SDL thread only: the current deferred switch already spent its one hidden
     * reconnect on this slot. A second no-AU deadline then means the stream does not
     * feed on_video_au at all (RemoteFX bitmap path) — retrying reconnects forever
     * would strand the switch, so the press is answered old-style instead. */
    bool snapshot_retry_used;
    /* Which graphics path this slot's stream last used (worker writes on every frame,
     * SDL thread reads). A bitmap-path slot cannot be snapshot-switched (no AUs to
     * cache) and must not be mistaken for a refresh-ineffective H.264 server: it
     * switches immediately the old way — resume simply restarts bitmap updates. */
    atomic_bool video_via_bitmap;
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
    /* HELLOLG_SNAPSHOT_FORCE=1: IDR-snapshot backgrounding for every slot, not only
     * the learned refresh-ineffective ones (device experiment knob; set once in main). */
    bool snapshot_force;
    /* SDL thread only: a color-key switch whose target stream is not ready yet (-1 =
     * none). The screen stays on the LIVE current session while the target fills its AU
     * snapshot in the background (resume+refresh, or a hidden reconnect); the switch
     * tick completes the switch by replay once the cache is ready and quiet — the user
     * never watches a black reload window. */
    int pending_switch_target;
    /* SDL thread only: when the deferred switch gives up and answers old-style. */
    uint32_t pending_switch_deadline_ticks;
    /* Mutable bitmap canvas for exactly one stream generation. RemoteFX updates are
     * dirty rectangles, so reusing it across either a slot switch or a same-slot
     * reconnect would expose pixels from the previous desktop. Guarded by video_lock. */
    NativeRgbaSurface *rgba;
    int rgba_owner_slot;
    unsigned rgba_owner_epoch;
    /* Frozen RemoteFX desktop that was below HUB when an asynchronous replacement
     * started. The replacement renders into `rgba`; this separate owner-tagged canvas
     * remains the compositor background and BACK fallback until that exact replacement
     * generation reaches ACTIVE and decodes a frame. Its SDL texture is destroyed only
     * on the SDL thread. Guarded by video_lock. */
    NativeRgbaSurface *hub_return_rgba;
    int hub_return_rgba_owner_slot;
    unsigned hub_return_rgba_owner_epoch;
    int hub_return_replacement_slot;
    unsigned hub_return_replacement_epoch;
    unsigned hub_return_replacement_baseline_frames;
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
    /* The single mixed-PCM audio track; all sessions flow through the fixed 48 kHz stereo
     * engine into it (video_lock). */
    NativeAudio *audio;
    /* Headless miniaudio engine plus lock-free per-session adaptive sources. Its pump
     * thread feeds app->audio; the engine format never follows a source format. */
    NativeAudioPipeline audio_pipeline;
    uint16_t audio_codec;        /* NATIVE_AUDIO_CODEC_*: auto (Opus) or lossless PCM */
    /* Per-slot fader position in dB (NATIVE_MIXER_FADER_MIN_DB..MAX_DB, 3 dB steps,
     * default 0 = unity; the bottom stop mutes). Only the SDL thread writes it (volume
     * overlay). The pipeline gain target is atomic and survives source reopen. */
    int8_t mixer_gain_db[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Notification ducking (SDL thread): duck_mask[i] = which slots' audio ducks slot i
     * -12 dB while slot i is on screen (bit = slot index, own bit ignored). Mirrors the
     * per-session settings duck_mask; the settings pointer is the persistence target for
     * the overlay's per-channel duck buttons. */
    uint8_t duck_mask[NATIVE_SETTINGS_MAX_SESSIONS];
    /* Console M/S state (SDL thread; bit = slot). Runtime-only like the faders — never
     * persisted; mute wins over solo inside the pipeline. */
    uint8_t mixer_mute_mask;
    uint8_t mixer_solo_mask;
    NativeSettings *settings;
    /* System-volume bridge for the mixer overlay's MASTER fader (luna_volume.h): the
     * fader mirrors and drives the webOS volume, never the app's own mix. */
    NativeLunaVolume luna_volume;
    pthread_mutex_t video_lock;
    /* Guards slot config strings between the SDL thread overwriting a slot's config on
     * (re)Connect and OTHER slots' rdp-workers scanning every config in
     * redact_if_sensitive. A slot's own worker is joined before its config is written,
     * but that never protected the cross-slot scan. */
    pthread_mutex_t redaction_lock;
    NativeInput input;
#if HELLOLG_WITH_EVDEV_INPUT
    /* Raw evdev mouse+keyboard reader (active during streaming); one background thread polls
     * grabbed /dev/input devices and wakes the SDL loop through eventfd. */
    NativeEvdevInput evdev_input;
#endif
    int decoder_errors;
    bool decoder_keyframe_pending;
    /* Set once the SDL graphics layer has presented a single transparent frame so the
     * NDL hardware video plane underneath shows through. Re-presenting a transparent
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
     * source live. */
    bool mixer_overlay_visible;
    int mixer_overlay_selected;
    uint32_t mixer_overlay_hide_ticks;
    /* Left button held on a fader: motion drags the selected knob (SDL thread only). */
    bool mixer_overlay_dragging;
    /* Pointer button pressed outside the floating console. Dismiss on its release so
     * that release is consumed before the evdev grab and RDP input are restored. */
    uint8_t mixer_overlay_dismiss_button;
    /* OK/ENTER currently held (SDL thread only): evdev autorepeat arrives as extra
     * down events, and activation must fire once per press, not once per repeat —
     * a repeated activate would XOR the duck bit and rewrite settings every time. */
    bool mixer_overlay_ok_held;
    /* Last system volume seen by the auto-raise detector (-1 = no baseline yet) and the
     * next poll tick: com.webos.audio idles out between requests and drops
     * subscriptions, so the cache is kept live by polling getVolume over LS2 —
     * in-process calls, no fork, safe next to the video pipeline. */
    int system_volume_seen;
    /* Luna reply_seq recorded when streaming was (re)entered: the baseline may only be
     * taken from a reply newer than this — the cache itself can predate streaming. */
    unsigned system_volume_baseline_seq;
    uint32_t system_volume_poll_ticks;
    /* Next system-volume poll while the overlay is visible (the MASTER knob follows
     * remote/headphone volume changes by reading, not by trusting bus events). */
    uint32_t mixer_overlay_volume_poll_ticks;
    /* SDL thread only: an opaque switch splash currently covers the video plane (drawn
     * while the keyframe watchdog is armed, i.e. a swap is in flight; the pipeline
     * reload window would otherwise show through as a black screen). */
    bool switch_splash_drawn;
    /* SDL thread only: latched when the streaming screen's side effects ran (UI hidden,
     * evdev grabbed); cleared when the screen returns to a configurator. */
    bool streaming_visible;
    /* SDL thread only: the user explicitly opened HUB over a still-active session.
     * This keeps the normal ACTIVE-state auto-entry from immediately hiding HUB again. */
    bool hub_visible;
    /* A grabbed remote confirm press opens HUB on its release. Keeping both edges in
     * evdev prevents the compositor from inheriting an orphaned release when HUB drops
     * the grab (some webOS remotes translate that orphan into BACK). */
    bool hub_open_key_held;
    /* Session that owned the video plane when HUB opened. BACK returns here even if a
     * nested setup form temporarily selected or backgrounded another slot. */
    int hub_return_slot;
    /* Async Connect/Save-and-connect started from HUB. HUB stays logically open until
     * this slot reaches ACTIVE, so a failure still lets BACK resume hub_return_slot. */
    int hub_connect_target;
    /* SDL-thread logical-session clocks for the HUB's per-slot "Session N min"
     * metadata. A hidden snapshot/watchdog reconnect belongs to the same user session,
     * so its transient non-ACTIVE state must not restart this 64-bit monotonic clock. */
    uint64_t session_started_ms[NATIVE_SETTINGS_MAX_SESSIONS];
    bool session_runtime_active[NATIVE_SETTINGS_MAX_SESSIONS];
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
    /* Which slot published the warp: the drain drops it when it no longer matches the
     * active slot — an outgoing worker can pass its active check right before a switch
     * and publish afterwards, and its desktop coordinates are meaningless (and visibly
     * jumpy) mapped through the NEW session's dimensions. */
    atomic_int pointer_warp_slot;
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

static NativeSessionSlot *native_active_slot(App *app) {
    return &app->sessions[atomic_load(&app->active_index)];
}

static bool native_slot_is_active(const NativeSessionSlot *slot) {
    return atomic_load(&slot->app->active_index) == slot->index;
}

/* A slot may take the screen while ACTIVE, or while its hidden reconnect is filling a
 * snapshot for a deferred switch. Keep this predicate in one place: navigation must
 * never select a slot whose worker has already reported a terminal failure. */
static bool native_slot_is_live_stream_target(const NativeSessionSlot *slot) {
    return slot && slot->rdp && !atomic_load(&slot->session_failed) &&
           (atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE || slot->snapshot_pending);
}

/* Points the pipeline's duck controller at the on-screen session with that session's
 * own trigger mask (SDL thread; every active_index store routes through here right after
 * the flip) — switching screens restores the combination set for the new session. */
static void native_duck_retarget(App *app) {
    int fg = atomic_load(&app->active_index);
    native_audio_pipeline_set_duck_foreground(&app->audio_pipeline, fg,
                                              (uint32_t)app->duck_mask[fg] & ~(1u << fg));
}

/* Monotonic milliseconds for cross-thread timing where SDL_GetTicks is unavailable
 * (worker callbacks compile without SDL). Session runtime uses the full 64-bit value;
 * the narrow wrapper below is only for short differences, where subtraction is
 * intentionally wrap-safe. */
static uint64_t native_monotonic_ms64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t native_monotonic_ms(void) {
    return (uint32_t)native_monotonic_ms64();
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
        clog(cLogLevelError, "config field %s is too long", field);
        return false;
    }
    memcpy(dest, value, len + 1);
    return true;
}

static bool native_session_endpoint_changed(const NativeSessionConfig *before,
                                            const NativeSessionConfig *after) {
    return strcmp(before->host, after->host) != 0 || before->port != after->port;
}

static bool native_session_connection_config_changed(const NativeSessionConfig *before,
                                                      const NativeSessionConfig *after) {
    return native_session_endpoint_changed(before, after) ||
           strcmp(before->username, after->username) != 0 ||
           strcmp(before->password, after->password) != 0 ||
           strcmp(before->domain, after->domain) != 0 || before->fps != after->fps;
}

/* Stopping an RDP worker is synchronous: rdp_session_stop() joins it. A worker that is
 * still resolving/connecting can be inside an uninterruptible libc/TCP timeout, so UI
 * actions must reject that transition instead of joining it on the SDL thread. */
static bool native_session_is_still_connecting(const NativeSessionSlot *slot) {
    return slot && slot->rdp && atomic_load(&slot->current_state) != (int)RDP_STATE_ACTIVE;
}

static void native_prepare_webos_environment(void) {
    if (!getenv("EGL_PLATFORM") && setenv("EGL_PLATFORM", "wayland", 0) != 0) {
        clog(cLogLevelWarning, "failed to set EGL_PLATFORM=wayland: %s", strerror(errno));
    }
    if (!getenv("XDG_RUNTIME_DIR") && setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0) != 0) {
        clog(cLogLevelWarning, "failed to set XDG_RUNTIME_DIR=/tmp/xdg: %s", strerror(errno));
    }
    clog(cLogLevelDebug, "webOS env APPID=%s EGL_PLATFORM=%s XDG_RUNTIME_DIR=%s",
         getenv("APPID") ? getenv("APPID") : "(unset)",
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
    }
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static bool g_sdl_runtime_initialized = false;

static int native_prepare_sdl_runtime(void) {
    if (!g_sdl_runtime_initialized) {
        if (SDL_Init(0) != 0) {
            clog(cLogLevelError, "SDL_Init(0) failed: %s", SDL_GetError());
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
        clog(cLogLevelError, "failed to parse %s", source);
        return false;
    }
    clog(cLogLevelInfo, "loaded %s", source);
    *applied = true;
    return true;
}

/* Reads the whole file at `path` into a NUL-terminated heap buffer with a single open.
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
        clog(cLogLevelError, "failed to inspect config file: %s", path);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > NATIVE_CONFIG_MAX_FILE) {
        fclose(file);
        clog(cLogLevelError, "config file is too large: %s", path);
        return false;
    }
    rewind(file);

    char *json = (char *)calloc((size_t)size + 5, 1);
    if (!json) {
        fclose(file);
        clog(cLogLevelError, "failed to allocate config buffer");
        return false;
    }

    size_t read_count = fread(json, 1, (size_t)size, file);
    fclose(file);
    if (read_count != (size_t)size) {
        free(json);
        clog(cLogLevelError, "failed to read config file: %s", path);
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
            clog(cLogLevelError, "config file not found: %s", path);
            return false;
        }
        if (log_missing) {
            clog(cLogLevelInfo, "config file not found at %s; using defaults and CLI overrides", path);
        }
        return true;
    }

    bool ok = native_settings_apply_json(settings, json, path);
    free(json);
    if (!ok) {
        clog(cLogLevelError, "failed to parse config file: %s", path);
        return false;
    }
    clog(cLogLevelInfo, "loaded config file: %s", path);
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
        clog(cLogLevelWarning, "failed to resolve SDL pref path: %s", SDL_GetError());
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
            clog(cLogLevelError, "persisted config path is too long");
            return false;
        }
        clog(cLogLevelInfo, "using persisted config path: %s", path);
        return true;
    }

    clog(cLogLevelWarning, "no writable persisted config directory found");
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
        clog(cLogLevelInfo,
             "skipped persisted config because saved settings were disabled for this launch");
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
            if (!native_config_parent_dir(candidates.paths[i], dir, sizeof(dir))) {
                continue;
            }
            if (!native_config_dir_secure_or_heal(dir)) {
                /* Never silent: a foreign-owned directory here made saved settings
                 * vanish without a trace in the log once already. */
                struct stat st;
                if (stat(dir, &st) == 0) {
                    clog(cLogLevelWarning,
                         "persisted-config candidate skipped (dir not private): %s uid=%lu mode=%lo", dir,
                         (unsigned long)st.st_uid, (unsigned long)(st.st_mode & 07777));
                }
                continue;
            }
        }

        NativeConfigLoadOutcome outcome = native_config_try_load_candidate(settings, candidates.paths[i]);
        if (outcome == NATIVE_CONFIG_LOAD_MISSING) {
            continue;
        }
        if (outcome == NATIVE_CONFIG_LOAD_INVALID) {
            clog(cLogLevelWarning, "ignored invalid persisted config: %s", candidates.paths[i]);
            continue;
        }
        clog(cLogLevelInfo, "loaded persisted config: %s", candidates.paths[i]);
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
        clog(cLogLevelError, "invalid string value for webOS launch field %s", key);
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
            clog(cLogLevelWarning, "webOS launch argument %d has no RDP config keys", i);
        }
        applied = applied || arg_applied;
    }
    if (saw_launch_json && !applied) {
        clog(cLogLevelWarning, "webOS launch parameters did not override RDP config");
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
        clog(cLogLevelError, "invalid value for %s", arg_name);
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
    clog(cLogLevelError, "invalid value for --audio-codec (expected auto or pcm)");
    return false;
}

static bool apply_cli_deprecated_audio_prebuffer(int argc, char **argv) {
    if (arg_value(argc, argv, "--audio-prebuffer-ms", NULL)) {
        native_settings_warn_deprecated_audio_prebuffer();
    }
    return true;
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
          apply_cli_deprecated_audio_prebuffer(argc, argv) &&
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
        clog(cLogLevelError, "invalid zero value in RDP config");
        ok = false;
    }
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        const NativeSessionConfig *session = &settings->sessions[slot];
        if (session->port == 0 || session->fps == 0) {
            clog(cLogLevelError, "invalid zero value in %s session config",
                 native_session_slot_name(slot));
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
        clog(cLogLevelError, "missing RDP host for the %s session", slot_name);
        ok = false;
    }
    if (!session->username[0]) {
        clog(cLogLevelError, "missing RDP username for the %s session", slot_name);
        ok = false;
    }
    if (!session->password[0]) {
        clog(cLogLevelError, "missing RDP password for the %s session", slot_name);
        ok = false;
    }
    if (session->port == 0 || session->fps == 0) {
        clog(cLogLevelError, "invalid zero value in %s session config", slot_name);
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
        clog(cLogLevelInfo,
             "effective %s RDP config host=%s port=%u username=%s password=%s domain=%s fps=%u",
             native_session_slot_name(slot), session->host, (unsigned)session->port,
             session->username[0] ? "set" : "missing", session->password[0] ? "set" : "missing",
             session->domain[0] ? "set" : "empty", (unsigned)session->fps);
    }
    clog(cLogLevelInfo,
         "effective globals desktop=%ux%u wheelStep=%u wheelScrollDivisor=%u audioCodec=%s",
         (unsigned)settings->width, (unsigned)settings->height, (unsigned)settings->wheel_step,
         (unsigned)settings->wheel_scroll_divisor,
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
    clog(cLogLevelInfo, "%s session state=%s(%d)%s%s",
         slot ? native_session_slot_name(slot->index) : "?", rdp_state_name(state), (int)state,
         safe_detail[0] ? " " : "", safe_detail);

    if (slot && rdp_state_is_terminal_error(state)) {
        clog(cLogLevelError, "terminal native error on the %s session: %s",
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
        clog(cLogLevelWarning,
             "leaking undrained RGBA texture to avoid cross-thread SDL_DestroyTexture");
    }
    app->pending_texture_destroy = stale;
}

/* SDL/main thread, while the renderer is still alive. The RDP workers may keep updating
 * CPU pixels until main joins them after native_run_app_loop(), but no render happens
 * after this point. Detach every renderer-owned texture so the later CPU-surface teardown
 * cannot call SDL_DestroyTexture after SDL_DestroyRenderer has invalidated it. */
static void native_destroy_rgba_renderer_textures(App *app) {
    if (!app) {
        return;
    }
    pthread_mutex_lock(&app->video_lock);
    SDL_Texture *rgba_texture = native_rgba_surface_take_texture(app->rgba);
    SDL_Texture *return_texture = native_rgba_surface_take_texture(app->hub_return_rgba);
    SDL_Texture *pending_texture = app->pending_texture_destroy;
    app->pending_texture_destroy = NULL;
    pthread_mutex_unlock(&app->video_lock);

    if (rgba_texture) {
        SDL_DestroyTexture(rgba_texture);
    }
    if (return_texture) {
        SDL_DestroyTexture(return_texture);
    }
    if (pending_texture) {
        SDL_DestroyTexture(pending_texture);
    }
}
#endif

/* video_lock protects both RGBA pointers and every owner/context field below. */
static bool native_rgba_owner_matches(const App *app, int slot, unsigned epoch) {
    return app && app->rgba && app->rgba_owner_slot == slot && app->rgba_owner_epoch == epoch;
}

static void native_reset_rgba_owner(App *app) {
    app->rgba_owner_slot = -1;
    app->rgba_owner_epoch = 0;
}

static void native_close_rgba_locked(App *app, bool defer_texture) {
    if (!app || !app->rgba) {
        if (app) {
            native_reset_rgba_owner(app);
        }
        return;
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    if (defer_texture) {
        native_defer_rgba_texture_destroy(app);
    }
#else
    (void)defer_texture;
#endif
    native_rgba_surface_close(app->rgba);
    app->rgba = NULL;
    native_reset_rgba_owner(app);
}

static void native_clear_hub_return_replacement(App *app) {
    app->hub_return_replacement_slot = -1;
    app->hub_return_replacement_epoch = 0;
    app->hub_return_replacement_baseline_frames = 0;
}

/* Main/SDL thread only: a return surface may own this renderer's texture, so close it
 * directly instead of consuming the worker-to-SDL pending texture handoff slot. */
static void native_close_hub_return_rgba_locked(App *app) {
    if (!app) {
        return;
    }
    native_rgba_surface_close(app->hub_return_rgba);
    app->hub_return_rgba = NULL;
    app->hub_return_rgba_owner_slot = -1;
    app->hub_return_rgba_owner_epoch = 0;
    native_clear_hub_return_replacement(app);
}

/* Freeze the frame that belongs to the session HUB promised BACK would return to.
 * A retry runs after active_index has already moved to the failed target, so it must
 * never replace this cache from old_index/current ownership. */
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static void native_capture_hub_return_rgba_locked(App *app) {
    if (!app || !app->hub_visible || app->hub_return_rgba) {
        return;
    }
    int return_slot = app->hub_return_slot;
    if (return_slot < 0 || return_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    unsigned return_epoch = atomic_load(&app->sessions[return_slot].connect_epoch);
    if (!native_rgba_owner_matches(app, return_slot, return_epoch) ||
        !native_rgba_surface_has_frame(app->rgba)) {
        return;
    }
    app->hub_return_rgba = app->rgba;
    app->hub_return_rgba_owner_slot = app->rgba_owner_slot;
    app->hub_return_rgba_owner_epoch = app->rgba_owner_epoch;
    app->rgba = NULL;
    native_reset_rgba_owner(app);
    native_clear_hub_return_replacement(app);
}

static void native_arm_hub_return_replacement_locked(App *app, int slot, unsigned epoch,
                                                      unsigned baseline_frames) {
    if (!app || !app->hub_return_rgba) {
        return;
    }
    app->hub_return_replacement_slot = slot;
    app->hub_return_replacement_epoch = epoch;
    app->hub_return_replacement_baseline_frames = baseline_frames;
}

/* BACK may resume the frozen canvas only while its worker generation is unchanged.
 * If backgrounding reconnected that slot, keep the old pixels as a read-only fallback;
 * its fresh generation will replace them after its first decoded frame. */
static bool native_promote_hub_return_rgba_locked(App *app, int slot, unsigned epoch) {
    if (!app || !app->hub_return_rgba || app->hub_return_rgba_owner_slot != slot ||
        app->hub_return_rgba_owner_epoch != epoch) {
        return false;
    }
    native_close_rgba_locked(app, false);
    app->rgba = app->hub_return_rgba;
    app->rgba_owner_slot = app->hub_return_rgba_owner_slot;
    app->rgba_owner_epoch = app->hub_return_rgba_owner_epoch;
    app->hub_return_rgba = NULL;
    app->hub_return_rgba_owner_slot = -1;
    app->hub_return_rgba_owner_epoch = 0;
    native_clear_hub_return_replacement(app);
    return true;
}
#endif

static void on_bitmap_update(void *ctx, uint16_t surface_id, uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                             uint32_t stride, const uint8_t *rgba, size_t len) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    /* Classify the graphics path BEFORE the background early-return: a backgrounded
     * stream (e.g. fresh from a hidden reconnect) must still teach the switch logic
     * that it renders via bitmaps and cannot be snapshot-switched. */
    atomic_store(&slot->video_via_bitmap, true);
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
    unsigned owner_epoch = atomic_load(&slot->connect_epoch);
    if (app->rgba && !native_rgba_owner_matches(app, slot->index, owner_epoch)) {
        /* A dirty rectangle can never be applied to another slot or connection
         * generation's pixels. Keep any frozen HUB return surface separate and start
         * this owner on a zeroed canvas. */
        native_close_rgba_locked(app, true);
    }
    if (app->video) {
        native_video_close(app->video);
        app->video = NULL;
        app->decoder_keyframe_pending = false;
        clog(cLogLevelNotice, "switching graphics path from NDL/H.264 to native RemoteFX RGBA");
    }
    if (!app->rgba) {
        app->rgba = native_rgba_surface_open(desktop_width, desktop_height);
        if (app->rgba) {
            app->rgba_owner_slot = slot->index;
            app->rgba_owner_epoch = owner_epoch;
        }
    } else if (native_rgba_surface_width(app->rgba) != desktop_width ||
               native_rgba_surface_height(app->rgba) != desktop_height) {
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
        native_defer_rgba_texture_destroy(app);
#endif
        if (native_rgba_surface_resize(app->rgba, desktop_width, desktop_height) != NATIVE_RGBA_OK) {
            native_rgba_surface_close(app->rgba);
            app->rgba = NULL;
            native_reset_rgba_owner(app);
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
        clog(cLogLevelError,
             "terminal native error: DecoderError; RemoteFX bitmap update failed result=%d surface=%u "
             "rect=%ux%u+%u+%u",
             (int)result, (unsigned)surface_id, (unsigned)width, (unsigned)height, (unsigned)left,
             (unsigned)top);
        slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
}

static bool on_log_enabled(void *ctx, RdpLogLevel level, const char *target) {
    (void)ctx;
    (void)target;
    return native_rdp_log_is_enabled(level);
}

static void on_log(void *ctx, RdpLogLevel level, const char *target, const char *message) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    App *app = slot ? slot->app : NULL;
    native_rdp_log_emit(level, slot ? native_session_slot_name(slot->index) : "?", target,
                        redact_if_sensitive(app, message));
}

/* Fires on the initial MCS/GCC handshake and again on every RDPGFX_RESET_GRAPHICS_PDU, so
 * the real EGFX graphics output size (which can differ from the negotiated session size)
 * stays current for both the NDL/H.264 and RemoteFX RGBA paths and for pointer mapping. */
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
    clog(cLogLevelInfo, "%s desktop=%ux%u", native_session_slot_name(slot->index), (unsigned)width,
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

/* NDL sink: the headless miniaudio engine renders one 10 ms S16 block and this
 * callback feeds NDL. The engine render itself is lock-free; only the shared NDL track
 * needs the existing media lock. */
static void native_audio_pipeline_feed_cb(void *ctx, const int16_t *samples, size_t frames) {
    App *app = (App *)ctx;
    pthread_mutex_lock(&app->video_lock);
    if (app->audio) {
        size_t bytes = frames * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(int16_t);
        if (native_audio_feed(app->audio, (const uint8_t *)samples, bytes) == NATIVE_AUDIO_ERROR) {
            /* Mute instead of closing: removing audio would reload the live video track
             * and force another server keyframe request. */
            clog(cLogLevelWarning, "mixed audio feed failed; muting audio");
            native_audio_disable(app->audio);
        }
    }
    pthread_mutex_unlock(&app->video_lock);
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
    if (!app || app->audio || !app->media || !native_audio_pipeline_is_initialized(&app->audio_pipeline)) {
        return;
    }
    app->audio = native_audio_open(app->media, RDP_AUDIO_CODEC_PCM_S16LE, NATIVE_AUDIO_PIPELINE_SAMPLE_RATE,
                                   NATIVE_AUDIO_PIPELINE_CHANNELS);
    if (app->audio) {
        clog(cLogLevelInfo, "opened speculative mixed PCM %uHz %uch track ahead of negotiation",
             NATIVE_AUDIO_PIPELINE_SAMPLE_RATE, NATIVE_AUDIO_PIPELINE_CHANNELS);
    }
}

static void on_video_au(void *ctx, const uint8_t *data, size_t len, bool is_keyframe, uint64_t pts90k) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot) {
        return;
    }
    App *app = slot->app;
    /* This stream feeds H.264 AUs: snapshot switching applies. Stamped before any
     * routing so a backgrounded stream classifies its slot too. */
    atomic_store(&slot->video_via_bitmap, false);
    if (!native_slot_is_active(slot)) {
        /* Background session: normally the server has been asked to suppress graphics —
         * this also covers the in-flight tail and servers that ignore
         * TS_SUPPRESS_OUTPUT_PDU. With the AU snapshot armed (hidden reconnect for a
         * cacheable IDR) the compressed bytes are kept for replay on switch-to; without
         * it they are dropped outright. */
        pthread_mutex_lock(&app->video_lock);
        if (native_slot_is_active(slot)) {
            /* Promoted while we waited on the lock (a switch just took/cleared the
             * snapshot): this AU belongs to the LIVE stream now — fall through to the
             * active path below instead of silently losing it. */
            pthread_mutex_unlock(&app->video_lock);
        } else {
            if (slot->snapshot.armed) {
                if (!native_au_snapshot_append(&slot->snapshot, data, len, is_keyframe, pts90k)) {
                    clog(cLogLevelWarning,
                         "%s AU snapshot overflowed (server still streaming?); switch-to will reconnect",
                         native_session_slot_name(slot->index));
                }
                /* The quiet-gate timestamp must be public BEFORE readiness: a reader
                 * seeing ready=true with the previous (stale) timestamp could judge the
                 * stream quiet at the very moment this AU is landing. */
                atomic_store(&slot->snapshot_last_au_ms, native_monotonic_ms());
                atomic_store(&slot->snapshot_idr_ready, native_au_snapshot_ready(&slot->snapshot));
            }
            pthread_mutex_unlock(&app->video_lock);
            return;
        }
    }
    if (!data || len == 0) {
        app->decoder_errors++;
        slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
        return;
    }

    /* The slot's desktop size reflects the server's real EGFX graphics output size
     * (on_desktop_size is re-invoked on every RDPGFX_RESET_GRAPHICS_PDU, not just the
     * initial MCS/GCC handshake), which can differ from the negotiated session size, e.g.
     * a TV whose hardware decoder always runs at panel resolution. Reopen the hardware decoder
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
        native_close_rgba_locked(app, true);
        clog(cLogLevelNotice, "switching graphics path from native RemoteFX RGBA to NDL/H.264");
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
                clog(cLogLevelDebug,
                     "waiting for a %s keyframe; %u delta AUs dropped since the switch",
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
            clog(cLogLevelNotice, "handing the video decoder to the %s session in-band (%ux%u)",
                 native_session_slot_name(slot->index), (unsigned)desktop_width,
                 (unsigned)desktop_height);
            app->video_owner_slot = slot->index;
            app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            app->decoder_keyframe_pending = false;
        } else {
            clog(cLogLevelNotice,
                 "swapping video to the %s session (keyframe in hand, %ux%u -> %ux%u)",
                 native_session_slot_name(slot->index), (unsigned)native_video_width(app->video),
                 (unsigned)native_video_height(app->video), (unsigned)desktop_width,
                 (unsigned)desktop_height);
            native_video_close(app->video);
            app->video = NULL;
            app->decoder_keyframe_pending = false;
        }
    }
    if (app->video &&
        (desktop_width != native_video_width(app->video) || desktop_height != native_video_height(app->video))) {
        clog(cLogLevelNotice, "hardware surface size changed %ux%u -> %ux%u; reopening decoder",
             (unsigned)native_video_width(app->video), (unsigned)native_video_height(app->video),
             (unsigned)desktop_width, (unsigned)desktop_height);
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
            app->video = native_video_open(media, desktop_width, desktop_height, slot->config.fps);
            if (app->video) {
                app->video_owner_slot = slot->index;
                app->video_owner_epoch = atomic_load(&slot->connect_epoch);
            }
        }
        if (!app->video) {
            pthread_mutex_unlock(&app->video_lock);
            app->decoder_errors++;
            clog(cLogLevelError,
                 "terminal native error: DecoderError; hardware decoder unavailable");
            slot_stop_with_state(slot, RDP_STATE_DECODER_ERROR, rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
            return;
        }
    }

    /* The callback byte lifetime is synchronous: do not retain data beyond native_video_feed.
     * If this callback later queues AUs to another thread, copy the bytes before returning.
     */
    NativeVideoResult result = native_video_feed(app->video, data, len, is_keyframe, pts90k);
    if (result == NATIVE_VIDEO_NEED_KEYFRAME) {
        /* Published while still holding video_lock and only for the slot that is active
         * RIGHT NOW: a switch flips active_index (and clears this flag) under the same
         * lock, so a feed racing the switch cannot leave a stale request behind — the
         * tick would send that refresh to the fresh target and arm its watchdog into a
         * needless reconnect on a static desktop. */
        if (slot->index == atomic_load(&app->active_index) && !app->decoder_keyframe_pending) {
            clog(cLogLevelWarning,
                 "decoder requested keyframe/recovery; waiting for native RDP recovery, no web fallback");
            app->decoder_keyframe_pending = true;
            /* Kick the SDL thread: refresh-capable servers deliver an IDR, refresh-
             * ineffective ones fall to the keyframe watchdog's reconnect. Without this a
             * broken reference chain (e.g. a stale snapshot replay) would freeze the
             * stream forever — grd never resends an IDR on its own. */
            atomic_store(&app->video_refresh_needed, true);
        }
        pthread_mutex_unlock(&app->video_lock);
        return;
    }
    pthread_mutex_unlock(&app->video_lock);
    if (result == NATIVE_VIDEO_OK) {
        app->decoder_keyframe_pending = false;
        /* Signals the SDL thread that the freshly switched-to stream is decoding. */
        atomic_fetch_add(&slot->video_ok_frames, 1u);
        return;
    }
    if (result == NATIVE_VIDEO_DROPPED) {
        /* Discarded by a still-loading decoder: not progress (video_ok_frames must not
         * move — the snapshot-replay success check and the switch watchdog read it as
         * "frames actually consumed") but not an error either. */
        return;
    }

    app->decoder_errors++;
    clog(cLogLevelError, "terminal native error: DecoderError; decoder feed result=%d", (int)result);
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
    slot->audio_routed = false;
    atomic_store(&slot->audio_codec, 0u);
    atomic_store(&slot->audio_sample_rate, 0u);
    atomic_store(&slot->audio_channels, 0u);
    if (codec != RDP_AUDIO_CODEC_PCM_S16LE && codec != RDP_AUDIO_CODEC_OPUS) {
        if (!slot->audio_incompatible_logged) {
            clog(cLogLevelWarning, "%s session negotiated unsupported codec %u; muting it in the mix",
                 native_session_slot_name(slot->index), (unsigned)codec);
            slot->audio_incompatible_logged = true;
        }
        native_audio_pipeline_close_source(&app->audio_pipeline, slot->index);
        return;
    }
    if (codec == RDP_AUDIO_CODEC_OPUS) {
        /* Fresh stream (or renegotiation): restart decoder state. Safe against
         * on_audio_data — both run on this session's own rdp-worker thread. */
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = native_opus_decoder_open(sample_rate, channels);
        if (!slot->opus_decoder) {
            if (!slot->audio_incompatible_logged) {
                clog(cLogLevelWarning, "%s session: no Opus decoder available; muting it in the mix",
                     native_session_slot_name(slot->index));
                slot->audio_incompatible_logged = true;
            }
            native_audio_pipeline_close_source(&app->audio_pipeline, slot->index);
            return;
        }
    } else if (slot->opus_decoder) {
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = NULL;
    }

    if (!native_audio_pipeline_set_source_format(&app->audio_pipeline, slot->index, sample_rate, channels)) {
        if (!slot->audio_incompatible_logged) {
            clog(cLogLevelWarning, "%s session has unsupported PCM format %uHz %uch; muting it",
                 native_session_slot_name(slot->index), (unsigned)sample_rate, (unsigned)channels);
            slot->audio_incompatible_logged = true;
        }
        native_opus_decoder_close(slot->opus_decoder);
        slot->opus_decoder = NULL;
        native_audio_pipeline_close_source(&app->audio_pipeline, slot->index);
        return;
    }
    atomic_store(&slot->audio_codec, codec);
    atomic_store(&slot->audio_sample_rate, sample_rate);
    atomic_store(&slot->audio_channels, channels);
    slot->audio_routed = true;
    slot->audio_incompatible_logged = false;
    clog(cLogLevelInfo, "%s session audio joined the mix (%s %uHz %uch)",
         native_session_slot_name(slot->index), codec == RDP_AUDIO_CODEC_OPUS ? "Opus->PCM" : "PCM",
         (unsigned)sample_rate, (unsigned)channels);

    pthread_mutex_lock(&app->video_lock);
    if (!app->audio) {
        /* First working audio format and the speculative open didn't happen (or failed):
         * bring the shared track up now. */
        NativeMedia *media = native_ensure_media_locked(app);
        if (media) {
            app->audio = native_audio_open(media, RDP_AUDIO_CODEC_PCM_S16LE,
                                           NATIVE_AUDIO_PIPELINE_SAMPLE_RATE,
                                           NATIVE_AUDIO_PIPELINE_CHANNELS);
        }
        if (app->audio) {
            if (app->video) {
                /* First-time open under a live video stream reloads the shared pipeline
                 * (webOS); drop the dead video track so on_video_au reopens it on the next
                 * SPS+PPS+IDR instead of feeding P-frames into a fresh decoder — and ask
                 * the SDL thread to force that keyframe (gnome-remote-desktop never
                 * resends an IDR spontaneously). */
                clog(cLogLevelNotice,
                     "audio open reloaded the media pipeline; reopening video on next keyframe");
                native_video_close(app->video);
                app->video = NULL;
                app->decoder_keyframe_pending = false;
                atomic_store(&app->video_refresh_needed, true);
            }
        } else {
            clog(cLogLevelWarning,
                 "audio sink unavailable (PCM 48000Hz 2ch); continuing with silent video");
        }
    }
    pthread_mutex_unlock(&app->video_lock);
}

static void on_audio_data(void *ctx, const uint8_t *data, size_t len, uint32_t ts_ms) {
    NativeSessionSlot *slot = (NativeSessionSlot *)ctx;
    if (!slot || !data || len == 0 || !slot->audio_routed) {
        return;
    }
    App *app = slot->app;
    if (slot->opus_decoder) {
        const int16_t *pcm = NULL;
        int frames = native_opus_decoder_decode(slot->opus_decoder, data, len, &pcm);
        if (frames > 0) {
            (void)native_audio_pipeline_push(&app->audio_pipeline, slot->index, pcm, (size_t)frames, ts_ms);
        }
        return;
    }
    size_t frame_bytes = (size_t)atomic_load(&slot->audio_channels) * sizeof(int16_t);
    size_t frames = frame_bytes ? len / frame_bytes : 0;
    if (frames > 0) {
        (void)native_audio_pipeline_push(&app->audio_pipeline, slot->index,
                                         (const int16_t *)(const void *)data, frames, ts_ms);
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
    atomic_store(&app->pointer_warp_slot, slot->index);
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
    native_reset_rgba_owner(app);
    /* native_stop_media() is SDL/main-thread only, so the frozen surface's texture can
     * be destroyed directly along with the current canvas. */
    native_close_hub_return_rgba_locked(app);
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

static void native_publish_effective_solo_mask(App *app) {
    uint8_t connected_mask = 0;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        if (app->sessions[i].rdp) {
            connected_mask |= (uint8_t)(1u << i);
        }
    }
    native_audio_pipeline_set_solo_mask(&app->audio_pipeline, app->mixer_solo_mask & connected_mask);
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
    pthread_mutex_lock(&app->video_lock);
    native_audio_pipeline_close_source(&app->audio_pipeline, index);
    slot->audio_routed = false;
    atomic_store(&slot->audio_codec, 0u);
    atomic_store(&slot->audio_sample_rate, 0u);
    atomic_store(&slot->audio_channels, 0u);
    slot->audio_incompatible_logged = false;
    /* Whatever the cache held belongs to the connection that just died. */
    native_au_snapshot_reset(&slot->snapshot);
    pthread_mutex_unlock(&app->video_lock);
    native_publish_effective_solo_mask(app);
    atomic_store(&slot->snapshot_idr_ready, false);
    slot->snapshot_pending = false;
    /* The worker is joined above; its decoder can be freed from this thread now. */
    native_opus_decoder_close(slot->opus_decoder);
    slot->opus_decoder = NULL;
    slot->suppressed = false;
    atomic_store(&slot->current_state, (int)RDP_STATE_IDLE);
    atomic_store(&slot->session_failed, false);
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    app->session_started_ms[index] = 0;
    app->session_runtime_active[index] = false;
#endif
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
 * connect-on-demand must not disturb the running stream. With `arm_snapshot` the AU
 * snapshot is armed HERE, between stopping the old worker and starting the new one —
 * the only ordering that guarantees the fresh connection cannot emit its IDR into an
 * unarmed cache (the caller re-reads snapshot.armed for the allocation-failure case). */
static bool native_slot_connect(App *app, int index, bool arm_snapshot) {
    if (!app || index < 0 || index >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }
    NativeSessionSlot *slot = &app->sessions[index];
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    /* All calls that reach here with a running slot are internal recovery reconnects,
     * except the explicit UI Connect path, which clears this clock before calling us. */
    uint64_t logical_session_started_ms = app->session_started_ms[index];
    bool logical_session_runtime_active = app->session_runtime_active[index];
#endif
    native_stop_slot(app, index);
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    pthread_mutex_lock(&app->video_lock);
    if (app->hub_return_rgba && app->hub_return_replacement_slot == index) {
        /* The old worker is joined, so this cumulative frame baseline is stable. Publish
         * the next epoch before the replacement worker can race out its first bitmap/AU. */
        native_arm_hub_return_replacement_locked(
            app, index, atomic_load(&slot->connect_epoch) + 1u,
            atomic_load(&slot->video_ok_frames));
    }
    pthread_mutex_unlock(&app->video_lock);
#endif
    if (arm_snapshot) {
        /* "Not ready" must be public before the cache can accept AUs (see on_video_au:
         * the timestamp/readiness pair is published append-side in the safe order). */
        atomic_store(&slot->snapshot_idr_ready, false);
        pthread_mutex_lock(&app->video_lock);
        if (!native_au_snapshot_arm(&slot->snapshot)) {
            clog(cLogLevelWarning, "%s AU snapshot allocation failed", native_session_slot_name(index));
        }
        pthread_mutex_unlock(&app->video_lock);
    }
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
        .on_log_enabled = on_log_enabled,
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

    clog(cLogLevelInfo, "starting %s session for %s:%u (%ux%u@%u AVC420/RemoteFX)",
         native_session_slot_name(index), config.host, (unsigned)config.port, (unsigned)config.width,
         (unsigned)config.height, (unsigned)config.fps);
    slot->rdp = rdp_session_start(&config, &callbacks);
    if (!slot->rdp) {
        clog(cLogLevelError, "rdp_session_start failed for the %s session",
             native_session_slot_name(index));
        return false;
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    app->session_started_ms[index] = logical_session_started_ms;
    app->session_runtime_active[index] = logical_session_runtime_active;
#endif
    native_publish_effective_solo_mask(app);
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
        clog(cLogLevelWarning, "SDL video displays unavailable: %s", SDL_GetError());
        return;
    }

    clog(cLogLevelDebug, "SDL video displays=%d", display_count);
    for (int display = 0; display < display_count; display++) {
        SDL_DisplayMode desktop_mode;
        if (SDL_GetDesktopDisplayMode(display, &desktop_mode) == 0) {
            clog(cLogLevelDebug, "SDL display %d desktop=%dx%d@%d format=0x%x", display,
                 desktop_mode.w, desktop_mode.h, desktop_mode.refresh_rate, (unsigned)desktop_mode.format);
        } else {
            clog(cLogLevelDebug, "SDL display %d desktop mode unavailable: %s", display, SDL_GetError());
        }

        SDL_DisplayMode current_mode;
        if (SDL_GetCurrentDisplayMode(display, &current_mode) == 0) {
            clog(cLogLevelDebug, "SDL display %d current=%dx%d@%d format=0x%x", display,
                 current_mode.w, current_mode.h, current_mode.refresh_rate, (unsigned)current_mode.format);
        } else {
            clog(cLogLevelDebug, "SDL display %d current mode unavailable: %s", display, SDL_GetError());
        }

        int mode_count = SDL_GetNumDisplayModes(display);
        if (mode_count < 0) {
            clog(cLogLevelDebug, "SDL display %d modes unavailable: %s", display, SDL_GetError());
            continue;
        }
        clog(cLogLevelTrace, "SDL display %d modes=%d", display, mode_count);
        for (int mode_index = 0; mode_index < mode_count; mode_index++) {
            SDL_DisplayMode mode;
            if (SDL_GetDisplayMode(display, mode_index, &mode) == 0) {
                clog(cLogLevelTrace, "SDL display %d mode %d=%dx%d@%d format=0x%x", display,
                     mode_index, mode.w, mode.h, mode.refresh_rate, (unsigned)mode.format);
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
        clog(cLogLevelError, "SDL renderer output size unavailable: %s", SDL_GetError());
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
        clog(cLogLevelDebug, "SDL renderer output=%dx%d", render_width, render_height);
        pthread_mutex_lock(&app->video_lock);
        native_media_set_viewport(app->media, clamped_width, clamped_height);
        pthread_mutex_unlock(&app->video_lock);
    }
    return true;
}

/* Input mapping uses the renderer OUTPUT size as the window size. On this target they
 * are the same surface: the webOS window is fullscreen at the compositor resolution and
 * is created without HIGHDPI, so SDL's logical window coordinates equal renderer output
 * pixels. A desktop/high-DPI port would have to map input from SDL_GetWindowSize
 * instead — mouse events and SDL_WarpMouseInWindow live in logical window coordinates,
 * not pixels. */
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
    if (atomic_load(&app->pointer_warp_slot) != atomic_load(&app->active_index)) {
        return; /* the outgoing session's warp, published across a switch: stale */
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
    clog(cLogLevelDebug, "SDL window size=%dx%d", current_width, current_height);
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

static bool native_sdl_confirm_key(const SDL_KeyboardEvent *event) {
    if (!event) {
        return false;
    }
    return event->keysym.sym == SDLK_RETURN || event->keysym.sym == SDLK_KP_ENTER ||
           event->keysym.sym == SDLK_RETURN2;
}

static void native_request_session_switch(App *app, int target);
static void native_ring_switch(App *app, int direction);
static void native_show_hub(App *app);
/* Volume-mixer overlay key hooks for the evdev drain (definitions live with the overlay
 * code, after the presenters they use). */
static bool native_mixer_overlay_consumes_evdev_key(uint16_t code);
static void native_mixer_overlay_evdev_key(App *app, uint16_t code, bool down);

/* Pre-connect screen: swallow session-navigation keys before the LVGL key queue eats
 * them. Onboarding temporarily owns the remote; after that, colours activate their
 * fixed profiles and CH +/- moves the hub selection. */
static int native_filter_webos_system_keys(void *userdata, SDL_Event *event) {
    App *app = (App *)userdata;
    if (!event || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)) {
        return 1;
    }
    int slot = native_sdl_webos_color_slot(&event->key);
    if (slot >= 0) {
        if (event->type == SDL_KEYDOWN && app) {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
            if (!native_preconnect_ui_session_keys_enabled(app->preconnect_ui)) {
                return 0;
            }
            native_preconnect_ui_cancel_pending_navigation(app->preconnect_ui);
#endif
            native_request_session_switch(app, slot);
        }
        return 0;
    }
    if (event->key.keysym.scancode == 480 /* SDL_WEBOS_SCANCODE_CH_UP */ ||
        event->key.keysym.scancode == 481 /* SDL_WEBOS_SCANCODE_CH_DOWN */) {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
        if (event->type == SDL_KEYDOWN && app && native_preconnect_ui_session_keys_enabled(app->preconnect_ui)) {
            (void)native_preconnect_ui_cycle_slot(app->preconnect_ui,
                                                  event->key.keysym.scancode == 480 ? 1 : -1);
        }
#endif
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
    clog_limited(cLogLevelDebug, 16, 1000, "unmapped evdev key %s code=%u",
                 down ? "down" : "up", (unsigned)code);
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

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
static bool native_evdev_confirm_key(uint16_t code) {
    return code == 28 /* KEY_ENTER */ || code == 96 /* KEY_KPENTER */ || code == 352 /* KEY_OK */;
}
#endif

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
            if (events[i].from_remote) {
                /* Remote presses are sparse; keep this diagnostic because key codes
                 * vary between webOS firmware and remote models. */
                clog(cLogLevelDebug, "remote key code=%u %s", (unsigned)events[i].code,
                     events[i].down ? "down" : "up");
            }
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
                /* Releases flow through too: the activation edge detector re-arms on
                 * the up edge (both edges stay swallowed either way). */
                native_mixer_overlay_evdev_key(app, events[i].code, events[i].down);
                continue;
            }
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
            if (events[i].from_remote && native_evdev_confirm_key(events[i].code)) {
                /* The central remote button owns HUB, but Enter from a physical USB
                 * keyboard (from_remote=false) remains an ordinary RDP key. Swallow
                 * both edges. Open on release so native_show_hub can drop EVIOCGRAB
                 * only after the compositor can no longer inherit the other half of
                 * this press and reinterpret it as BACK. Autorepeat merely keeps the
                 * held latch armed. */
                if (events[i].down) {
                    app->hub_open_key_held = true;
                } else {
                    bool opened_from_this_press = app->hub_open_key_held;
                    app->hub_open_key_held = false;
                    if (opened_from_this_press && app->streaming_visible) {
                        native_show_hub(app);
                    }
                }
                continue;
            }
#endif
            if (events[i].code == 402 /* KEY_CHANNELUP */ || events[i].code == 403 /* KEY_CHANNELDOWN */) {
                /* Channel-zapping between the connected slots; like the color keys these
                 * may ride either input path depending on the remote firmware. */
                if (events[i].down && app->streaming_visible) {
                    native_ring_switch(app, events[i].code == 402 ? 1 : -1);
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
            clog(cLogLevelWarning,
                 "input capture failed to start even though a USB keyboard is attached; this session has no "
                 "mouse or keyboard input");
            return NATIVE_INPUT_START_UNAVAILABLE;
        }
        clog(cLogLevelWarning,
             "no USB mouse/keyboard grabbed; using SDL mouse fallback and no keyboard input");
        return NATIVE_INPUT_START_NO_KEYBOARD;
    }
    if (!native_evdev_input_mouse_active(&app->evdev_input)) {
        clog(cLogLevelInfo,
             "no USB mouse to grab; using the compositor pointer (Magic Remote) via SDL");
    }
    if (!native_evdev_input_keyboard_active(&app->evdev_input)) {
        clog(cLogLevelWarning,
             "no USB keyboard grabbed and there is no SDL keyboard fallback; this session has no keyboard "
             "input until a USB keyboard is attached (it is picked up live)");
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
    app->hub_open_key_held = false;
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
    if (!app || !app->streaming_visible ||
        atomic_load(&native_active_slot(app)->current_state) != (int)RDP_STATE_ACTIVE) {
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
     * re-presenting every loop tick raced the NDL hardware video plane's own buffer
     * swaps on this webOS compositor and produced visible flicker between the (empty)
     * graphics layer and the video plane underneath. */
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    app->video_plane_punched = true;
    if (logged && !*logged) {
        clog(cLogLevelDebug, "presented initial SDL renderer frame");
        *logged = true;
    }
}

static void native_present_surface_frame(SDL_Window *window, bool *logged) {
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    if (!surface) {
        clog(cLogLevelError, "SDL_GetWindowSurface failed: %s", SDL_GetError());
        return;
    }
    if (SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, 0, 0, 0)) != 0) {
        clog(cLogLevelError, "SDL_FillRect launch frame failed: %s", SDL_GetError());
        return;
    }
    if (SDL_UpdateWindowSurface(window) != 0) {
        clog(cLogLevelError, "SDL_UpdateWindowSurface launch frame failed: %s", SDL_GetError());
        return;
    }
    if (logged && !*logged) {
        clog(cLogLevelDebug, "presented initial SDL surface frame");
        *logged = true;
    }
}

/* Draws the cached RemoteFX/bitmap desktop into the current window backbuffer without
 * presenting. Returns 1 for a completed draw, 2 for a recoverable skipped draw (the
 * previous front buffer remains owned by RGBA), 0 when no frame exists, and -1 for a
 * terminal failure. Streaming presents immediately below; HUB uses the same draw as its
 * bottom layer, then LVGL adds translucent chrome and performs the single present. */
static int native_draw_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    if (!app || !renderer) {
        return 0;
    }
    int status = 0;
    int failed_owner_slot = -1;
    NativeRgbaResult failed_result = NATIVE_RGBA_OK;
    pthread_mutex_lock(&app->video_lock);
    if (app->pending_texture_destroy) {
        SDL_DestroyTexture(app->pending_texture_destroy);
        app->pending_texture_destroy = NULL;
    }
    int active_index = atomic_load(&app->active_index);
    NativeSessionSlot *active = &app->sessions[active_index];
    unsigned active_epoch = atomic_load(&active->connect_epoch);
    bool active_rgba_ready = native_rgba_owner_matches(app, active_index, active_epoch) &&
                             native_rgba_surface_has_frame(app->rgba);
    bool active_h264_ready = app->video && app->video_owner_slot == active_index &&
                             app->video_owner_epoch == active_epoch;
    bool replacement_ready =
        app->hub_return_rgba && app->hub_return_replacement_slot == active_index &&
        app->hub_return_replacement_epoch == active_epoch &&
        atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE &&
        atomic_load(&active->video_ok_frames) != app->hub_return_replacement_baseline_frames &&
        (active_rgba_ready || active_h264_ready);
    bool drawing_return = app->hub_return_rgba && !replacement_ready;
    NativeRgbaSurface *surface = active_rgba_ready ? app->rgba : NULL;
    if (drawing_return) {
        surface = app->hub_return_rgba;
    }
    if (surface && native_rgba_surface_has_frame(surface)) {
        uint16_t viewport_width = (uint16_t)atomic_load(&app->render_width);
        uint16_t viewport_height = (uint16_t)atomic_load(&app->render_height);
        NativeRgbaResult result = native_rgba_surface_render(surface, renderer, viewport_width, viewport_height);
        if (result == NATIVE_RGBA_OK) {
            status = 1;
            /* RGBA presents opaque content into the same renderer the NDL hole-punch
             * uses. If the stream later switches back to H.264, the punch-through latch
             * must fire again so the SDL layer clears back to transparent instead of
             * leaving this opaque frame in front of the video plane. */
            app->video_plane_punched = false;
        } else if (result == NATIVE_RGBA_RETRY) {
            /* Do not fall through to the transparent NDL punch or present the cleared
             * backbuffer. The next tick retries from the cached CPU frame. */
            status = 2;
        } else {
            failed_result = result;
            failed_owner_slot = drawing_return ? app->hub_return_rgba_owner_slot : active_index;
            status = -1;
            if (drawing_return) {
                /* This frozen texture exhausted its renderer-recovery budget. Drop it
                 * after preserving the owner for one terminal report; otherwise every
                 * HUB tick would retry and re-report the same unusable cache forever. */
                native_close_hub_return_rgba_locked(app);
                drawing_return = false;
            }
        }
    }
    /* A successful software draw, or a hardware owner with one accepted AU, is the
     * replacement's first presentable frame. Retire the frozen desktop only now; a
     * transient SDL retry deliberately keeps it for the next tick. */
    if (app->hub_return_rgba && replacement_ready &&
        (active_h264_ready || (status == 1 && !drawing_return))) {
        native_close_hub_return_rgba_locked(app);
    }
    pthread_mutex_unlock(&app->video_lock);
    if (status == 1 && logged && !*logged) {
        clog(cLogLevelDebug, "presented initial native RemoteFX RGBA frame");
        *logged = true;
    } else if (status < 0) {
        app->decoder_errors++;
        clog(cLogLevelError,
             "terminal native error: DecoderError; RemoteFX RGBA present failed result=%d",
             (int)failed_result);
        NativeSessionSlot *failed_slot =
            failed_owner_slot >= 0 && failed_owner_slot < NATIVE_SETTINGS_MAX_SESSIONS
                ? &app->sessions[failed_owner_slot]
                : native_active_slot(app);
        slot_stop_with_state(failed_slot, RDP_STATE_DECODER_ERROR,
                             rdp_state_exit_code(RDP_STATE_DECODER_ERROR));
    }
    return status;
}

static int native_present_rgba_frame(App *app, SDL_Renderer *renderer, bool *logged) {
    int status = native_draw_rgba_frame(app, renderer, logged);
    if (status == 1) {
        SDL_RenderPresent(renderer);
    }
    return status;
}

static bool native_draw_preconnect_background(void *ctx, SDL_Renderer *renderer) {
    return native_draw_rgba_frame((App *)ctx, renderer, NULL) > 0;
}

static void native_set_slot_badge_color(SDL_Renderer *renderer, int slot) {
    uint32_t rgb = native_ui_slot_rgb(slot);
    SDL_SetRenderDrawColor(renderer, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 230);
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

/* Any interaction: push the auto-hide deadline out. */
static void native_mixer_overlay_touch(App *app) {
    app->mixer_overlay_hide_ticks = SDL_GetTicks() + NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS;
}

static void native_mixer_overlay_hide(App *app) {
    if (!app->mixer_overlay_visible) {
        return;
    }
    /* An outside click is closed on its release, not its press. Any simultaneous key,
     * auto-switch or teardown request must wait for that edge too; otherwise this
     * function would re-grab evdev while the physical pointer button is still down. */
    if (app->mixer_overlay_dismiss_button != 0) {
        return;
    }
    app->mixer_overlay_visible = false;
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    native_ui_mixer_hide(native_preconnect_ui_mixer(app->preconnect_ui));
    app->mixer_overlay_dragging = false;
    app->mixer_overlay_dismiss_button = 0;
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

/* A streaming teardown cannot wait for the outside-click button-up: once the
 * preconnect loop owns SDL events that edge no longer reaches the overlay handler.
 * The caller must clear streaming_visible first so this path never re-grabs input. */
static void native_mixer_overlay_force_hide(App *app) {
    if (!app) {
        return;
    }
    app->mixer_overlay_dismiss_button = 0;
    native_mixer_overlay_hide(app);
}

static void native_mixer_overlay_show(App *app) {
    pthread_mutex_lock(&app->video_lock);
    bool rgba_active = app->rgba != NULL;
    pthread_mutex_unlock(&app->video_lock);
    /* The panel renders through the LVGL preconnect display only. Without it the
     * overlay would hijack keys while drawing nothing, so use the badge instead. */
    bool mixer_available = false;
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    mixer_available = native_preconnect_ui_mixer(app->preconnect_ui) != NULL;
#endif
    if (rgba_active || !mixer_available) {
        /* The RemoteFX RGBA path repaints the whole renderer every frame, so the panel
         * would be invisible while its keys still hijack input: keep the badge flash. */
        native_show_session_indicator(app, atomic_load(&app->active_index));
        return;
    }
    app->mixer_overlay_visible = true;
    app->mixer_overlay_selected = atomic_load(&app->active_index);
    /* The close-by-OK release lands after hide, when the overlay no longer consumes it,
     * so the held latch can be stale here: re-arm activation for this panel session. */
    app->mixer_overlay_ok_held = false;
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
    app->mixer_overlay_dismiss_button = 0;
    native_stop_streaming_input(app);
    native_input_set_active(&app->input, false);
    native_cursor_show_default();
#endif
    /* Re-sync the MASTER fader with the platform: the remote's VOL keys or headphone
     * buttons may have moved the system volume since the overlay was last up. */
    native_luna_volume_refresh(&app->luna_volume);
    native_mixer_overlay_touch(app);
}

static void native_mixer_overlay_select(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_UI_MIXER_CHANNELS) {
        app->mixer_overlay_selected = slot;
    }
    native_mixer_overlay_touch(app); /* even an edge bump keeps the overlay alive */
}

/* MASTER fader edit: absolute system volume over the Luna bus. The optimistic cache in
 * luna_volume keeps the knob tracking key repeats; the worker coalesces the bus calls. */
static void native_mixer_overlay_set_master_pct(App *app, int pct) {
    native_luna_volume_set(&app->luna_volume, pct);
    native_mixer_overlay_touch(app);
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
        app->mixer_gain_db[slot] = (int8_t)gain_db;
        native_audio_pipeline_set_source_gain(&app->audio_pipeline, slot,
                                              native_ui_mixer_gain_db_to_q15(gain_db));
    }
    native_mixer_overlay_touch(app);
}

/* Duck-button press on a channel: flips whether that BACKGROUND session's audio ducks
 * the currently displayed session (per-foreground trigger mask), persists the new mask,
 * and retargets the pipeline's controller. The active channel's own button is inert. */
static void native_mixer_overlay_toggle_duck(App *app, int slot) {
    int fg = atomic_load(&app->active_index);
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS && slot != fg) {
        app->duck_mask[fg] ^= (uint8_t)(1u << slot);
        if (app->settings) {
            app->settings->sessions[fg].duck_mask = app->duck_mask[fg];
            (void)native_config_save_persisted(app->settings);
        }
        native_duck_retarget(app);
        clog(cLogLevelInfo, "duck trigger %s: %s while %s is on screen",
             (app->duck_mask[fg] >> slot) & 1u ? "enabled" : "disabled",
             native_session_slot_name(slot), native_session_slot_name(fg));
    }
    native_mixer_overlay_touch(app);
}

/* M plate click: mutes/unmutes that channel in the mix (any session slot, the active
 * one included). Runtime-only — nothing is persisted. */
static void native_mixer_overlay_toggle_mute(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        app->mixer_mute_mask ^= (uint8_t)(1u << slot);
        bool muted = (app->mixer_mute_mask >> slot) & 1u;
        native_audio_pipeline_set_source_muted(&app->audio_pipeline, slot, muted);
        clog(cLogLevelInfo, "%s %s", native_session_slot_name(slot), muted ? "muted" : "unmuted");
    }
    native_mixer_overlay_touch(app);
}

/* S plate click: toggles that channel's solo. While any solo is lit, only soloed
 * channels reach the mix (mute still wins on a muted+soloed channel). */
static void native_mixer_overlay_toggle_solo(App *app, int slot) {
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        app->mixer_solo_mask ^= (uint8_t)(1u << slot);
        native_publish_effective_solo_mask(app);
        clog(cLogLevelInfo, "solo %s: mask 0x%x", native_session_slot_name(slot),
             (unsigned)app->mixer_solo_mask);
    }
    native_mixer_overlay_touch(app);
}

/* OK/ENTER while the overlay is up: pressing a background session's channel flips its
 * duck button; on the active channel or the MASTER it keeps the old close behavior. */
static void native_mixer_overlay_activate(App *app) {
    int slot = app->mixer_overlay_selected;
    if (slot >= 0 && slot < NATIVE_SETTINGS_MAX_SESSIONS && slot != atomic_load(&app->active_index)) {
        native_mixer_overlay_toggle_duck(app, slot);
        return;
    }
    native_mixer_overlay_hide(app);
}

static void native_mixer_overlay_adjust(App *app, int delta_steps) {
    int slot = app->mixer_overlay_selected;
    if (slot == NATIVE_UI_MIXER_MASTER) {
        int pct = native_luna_volume_cached(&app->luna_volume);
        if (pct < 0) {
            /* Volume still unknown (probe in flight or bus unavailable): just re-ask. */
            native_luna_volume_refresh(&app->luna_volume);
            native_mixer_overlay_touch(app);
            return;
        }
        native_mixer_overlay_set_master_pct(app,
                                            pct + delta_steps / NATIVE_MIXER_OVERLAY_GAIN_STEP_DB *
                                                      NATIVE_UI_MIXER_MASTER_STEP_PCT);
        return;
    }
    native_mixer_overlay_set_db(app, slot, (int)app->mixer_gain_db[slot] + delta_steps);
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

static void native_mixer_overlay_evdev_key(App *app, uint16_t code, bool down) {
    if (!down) {
        if (code == 28 || code == 96 || code == 352) {
            app->mixer_overlay_ok_held = false; /* release edge re-arms activation */
        }
        return;
    }
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
    case 28:  /* KEY_ENTER */
    case 96:  /* KEY_KPENTER */
    case 352: /* KEY_OK */
        /* Edge-triggered, unlike the deliberately repeating fader arrows: evdev
         * autorepeat (value 2) folds into more down events. */
        if (!app->mixer_overlay_ok_held) {
            app->mixer_overlay_ok_held = true;
            native_mixer_overlay_activate(app);
        }
        break;
    default: /* esc/back */
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
        /* Edge-triggered like the evdev path: a held Enter repeats key-downs, and a
         * repeated activate would XOR the duck bit once per repeat. */
        if (event->repeat == 0) {
            native_mixer_overlay_activate(app);
        }
        break;
    case SDLK_ESCAPE:
        native_mixer_overlay_hide(app);
        break;
    default:
        break;
    }
}

/* Arms the auto-raise detector for a (re)entered streaming screen. The detector must
 * not baseline off the cached volume: nothing polls outside streaming, so the cache can
 * still hold a pre-streaming value — the first in-stream poll reply would then read as
 * an "external change" and pop the mixer unasked. Recording the reply sequence makes
 * the tick below wait for a reply that arrived AFTER this point. */
static void native_system_volume_rebaseline(App *app) {
    app->system_volume_seen = -1;
    app->system_volume_baseline_seq = native_luna_volume_reply_seq(&app->luna_volume);
}

/* Per-tick system-volume watcher. The 200ms getVolume poll keeps the cached volume
 * live, so this is mostly comparison. A change made outside the overlay (the remote's
 * VOL keys, headphone buttons) RAISES the mixer on the MASTER channel; while the
 * volume keeps moving the auto-hide timer keeps resetting, and once it settles the
 * normal idle timeout hides the panel. The user's own fader edits flow through the same
 * detector (optimistic cache updates count as changes) and simply keep the visible
 * overlay alive — same effect as any other interaction. */
static void native_system_volume_tick(App *app) {
    if (!app->streaming_visible) {
        return; /* the overlay is a streaming-screen surface */
    }
    uint32_t now = SDL_GetTicks();
    if (!app->mixer_overlay_dragging && SDL_TICKS_PASSED(now, app->system_volume_poll_ticks)) {
        /* Not while dragging: a poll reply from before the drag's newest set would
         * briefly yank the knob backwards. */
        app->system_volume_poll_ticks = now + NATIVE_SYSTEM_VOLUME_POLL_MS;
        native_luna_volume_refresh(&app->luna_volume);
    }
    int pct = native_luna_volume_cached(&app->luna_volume);
    if (pct < 0) {
        return;
    }
    if (app->system_volume_seen < 0) {
        if (native_luna_volume_reply_seq(&app->luna_volume) == app->system_volume_baseline_seq) {
            return; /* cache may predate this streaming entry; wait for a fresh reply */
        }
        app->system_volume_seen = pct; /* the first fresh reading is the baseline, not a change */
        return;
    }
    if (pct == app->system_volume_seen) {
        return;
    }
    clog(cLogLevelDebug, "system volume %d -> %d (overlay %s)", app->system_volume_seen, pct,
         app->mixer_overlay_visible ? "up" : "hidden");
    app->system_volume_seen = pct;
    if (app->mixer_overlay_visible) {
        native_mixer_overlay_touch(app); /* still moving: hold the panel open */
    } else {
        native_mixer_overlay_show(app);
        native_mixer_overlay_select(app, NATIVE_UI_MIXER_MASTER);
    }
}

/* Presents the live LVGL overlay; per-tick meter updates animate with playback. It is
 * cleared back to the punch frame on hide/expiry and skipped while the RemoteFX RGBA
 * path owns the renderer (native_mixer_overlay_show refuses to open there). */
static void native_present_mixer_overlay_frame(App *app, SDL_Renderer *renderer) {
    if (!app->mixer_overlay_visible) {
        return;
    }
    if (!app->mixer_overlay_dragging && app->mixer_overlay_dismiss_button == 0 &&
        SDL_TICKS_PASSED(SDL_GetTicks(), app->mixer_overlay_hide_ticks)) {
        native_mixer_overlay_hide(app);
        return;
    }
    /* Poll the system volume while the panel is on screen so the MASTER knob follows
     * the remote's VOL keys / headphone buttons. Not while dragging: a poll reply from
     * before the drag's newest set would briefly yank the knob backwards. */
    if (!app->mixer_overlay_dragging &&
        SDL_TICKS_PASSED(SDL_GetTicks(), app->mixer_overlay_volume_poll_ticks)) {
        app->mixer_overlay_volume_poll_ticks = SDL_GetTicks() + NATIVE_MIXER_OVERLAY_VOLUME_POLL_MS;
        native_luna_volume_refresh(&app->luna_volume);
    }
    unsigned connected_mask = 0;
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        if (slot->rdp && atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE) {
            connected_mask |= 1u << i;
        }
    }
    /* The MASTER channel is "connected" once the Luna bus has answered: its knob then
     * mirrors the live system volume (dimmed until the first round-trip lands). */
    int master_pct = native_luna_volume_cached(&app->luna_volume);
    if (native_luna_volume_available(&app->luna_volume) && master_pct >= 0) {
        connected_mask |= 1u << NATIVE_UI_MIXER_MASTER;
    }
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    NativeUiMixer *mixer_ui = native_preconnect_ui_mixer(app->preconnect_ui);
    if (mixer_ui) {
        /* Meter/stat snapshots are atomic and never enter the callback path. */
        int32_t peaks[NATIVE_UI_MIXER_CHANNELS][2] = {{0, 0}};
        unsigned queue_ms[NATIVE_SETTINGS_MAX_SESSIONS] = {0};
        unsigned target_ms[NATIVE_SETTINGS_MAX_SESSIONS] = {0};
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            NativeAudioSourceStats stats;
            native_audio_pipeline_get_source_peaks(&app->audio_pipeline, i, &peaks[i][0], &peaks[i][1]);
            if (native_audio_pipeline_get_source_stats(&app->audio_pipeline, i, &stats)) {
                queue_ms[i] = stats.queue_ms;
                target_ms[i] = stats.target_delay_ms;
            }
        }
        native_audio_pipeline_get_output_peaks(&app->audio_pipeline, &peaks[NATIVE_UI_MIXER_MASTER][0],
                                               &peaks[NATIVE_UI_MIXER_MASTER][1]);
        int duck_fg = atomic_load(&app->active_index);
        native_ui_mixer_render(mixer_ui, peaks, queue_ms, target_ms, app->mixer_gain_db, master_pct,
                               app->mixer_overlay_selected, connected_mask, duck_fg, app->duck_mask[duck_fg],
                               app->mixer_mute_mask, app->mixer_solo_mask, SDL_GetTicks());
        app->video_plane_punched = true; /* screen content is ours, not the punch frame */
        return;
    }
#endif
    /* No LVGL mixer: show() refuses to open the overlay, so this is unreachable. */
    (void)renderer;
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

#ifndef NATIVE_SWITCH_TEST_NO_RECONNECT
#define NATIVE_SWITCH_TEST_NO_RECONNECT 0
#endif

/* Sends `index` (the slot that owned the screen) to the background. A server that honors
 * keyframe requests is simply suppressed: switching back asks for a fresh IDR via the
 * Display Control layout resubmit. A refresh-ineffective server (grd <= 45, mirror mode)
 * emits its only IDR at connect time, so a plain suppress strands the slot behind an
 * unusable delta chain and switching back costs an ON-SCREEN reconnect. Instead,
 * reconnect it now — invisible behind the new active stream — and arm the AU snapshot:
 * on_video_au caches the fresh connect IDR and the switch tick suppresses the server
 * once it is in hand. Switching back then replays the cache instead of reconnecting. */
static void native_background_slot(App *app, int index) {
    NativeSessionSlot *slot = &app->sessions[index];
    if (!slot->rdp || slot->suppressed) {
        return;
    }
    bool snapshot_wanted = (slot->refresh_ineffective || app->snapshot_force) &&
                           atomic_load(&slot->current_state) == (int)RDP_STATE_ACTIVE &&
                           !NATIVE_SWITCH_TEST_NO_RECONNECT;
    if (snapshot_wanted) {
        clog(cLogLevelNotice, "reconnecting the %s session in the background to cache a fresh IDR",
             native_session_slot_name(index));
        /* The snapshot is armed inside the connect, between the old worker's teardown
         * and the new worker's start, so the connect IDR cannot race an unarmed cache. */
        if (!native_slot_connect(app, index, true)) {
            /* Startup failure: route it through the standard failure drain (per-slot
             * status line, quiet background teardown) instead of leaving a dead handle. */
            slot_stop_with_state(slot, RDP_STATE_NETWORK_ERROR, rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
            return;
        }
        pthread_mutex_lock(&app->video_lock);
        bool armed = slot->snapshot.armed;
        pthread_mutex_unlock(&app->video_lock);
        if (armed) {
            slot->snapshot_pending = true; /* the switch tick suppresses once the IDR lands */
            slot->snapshot_deadline_ticks = 0;
        } else {
            /* No memory for the cache: background it plainly; switching back takes the
             * usual refresh-or-reconnect path. */
            clog(cLogLevelWarning, "%s AU snapshot allocation failed; backgrounding without a cache",
                 native_session_slot_name(index));
            rdp_set_suppress_output(slot->rdp, false);
            slot->suppressed = true;
        }
        return;
    }
    /* Works for a still-CONNECTING slot too: the Rust worker buffers control commands
     * and replays them once the session goes active, so a slot left mid-handshake still
     * comes up backgrounded (suppressed) instead of streaming video nobody displays. */
    rdp_set_suppress_output(slot->rdp, false);
    slot->suppressed = true;
}

/* Rebuilds the shared decoder's reference state for `target` from its cached connect IDR
 * (+ raced deltas), feeding the raw AUs through the regular ingest path (on_video_au)
 * exactly as if they had just arrived from the network — including the same-size in-band
 * decoder handover, so a matching resolution swaps with no pipeline reload. SDL thread;
 * the slot must be ACTIVE (owns the screen) and still suppressed, so a compliant server
 * has no AU in flight that could interleave with the replay (resume is only sent after
 * it). A stream that was still audible within the quiet window — the suppress tail of a
 * fast flip-back, or a server that ignores suppress altogether — is refused: its
 * in-flight AUs would race the replay and corrupt the reference chain, so the switch
 * takes the deterministic reconnect fallback instead.
 * Returns true when the decoder accepted the replay. */
static bool native_snapshot_replay(App *app, int target) {
    NativeSessionSlot *slot = &app->sessions[target];
    if (!atomic_load(&slot->snapshot_idr_ready)) {
        return false;
    }
    NativeAuSnapshot snap;
    pthread_mutex_lock(&app->video_lock);
    snap = slot->snapshot; /* take ownership of the buffer */
    memset(&slot->snapshot, 0, sizeof(slot->snapshot));
    pthread_mutex_unlock(&app->video_lock);
    atomic_store(&slot->snapshot_idr_ready, false);
    if (!native_au_snapshot_ready(&snap)) {
        free(snap.buf);
        return false;
    }
    uint32_t since_last_au = native_monotonic_ms() - atomic_load(&slot->snapshot_last_au_ms);
    if (since_last_au < NATIVE_SNAPSHOT_QUIET_MS) {
        /* Ready implies at least one AU was cached, so the stamp is valid. The normal
         * suppress tail dies out within tens of ms of backgrounding, and a background
         * period lasts far longer than the quiet window — this fires only for a
         * flip-back racing the tail or a server that ignores TS_SUPPRESS_OUTPUT_PDU. */
        clog(cLogLevelDebug, "%s streamed %ums ago, may still have AUs in flight; skipping replay",
             native_session_slot_name(target), (unsigned)since_last_au);
        free(snap.buf);
        return false;
    }
    clog(cLogLevelDebug, "replaying %u cached %s AUs (%zu bytes) to re-enter the delta chain",
         snap.count, native_session_slot_name(target), snap.used);
    unsigned baseline = atomic_load(&slot->video_ok_frames);
    for (unsigned i = 0; i < snap.count; i++) {
        const NativeAuSnapshotEntry *entry = &snap.entries[i];
        on_video_au(slot, snap.buf + entry->offset, entry->len, entry->is_keyframe, entry->pts90k);
        if (!atomic_load(&app->running) || atomic_load(&slot->session_failed)) {
            break; /* a terminal feed error stopped the slot (or app); the drain owns it */
        }
    }
    free(snap.buf);
    if (atomic_load(&slot->video_ok_frames) == baseline) {
        clog(cLogLevelWarning, "%s snapshot replay fed no frames; falling back",
             native_session_slot_name(target));
        return false;
    }
    return true;
}

/* Finalizes a switch to `target`, which must already be connected. Retargets input,
 * cursor, and the shared video path while the mixed audio track stays attached, then
 * asks the servers to pause/resume their graphics. */
static void native_complete_session_switch(App *app, int target) {
    int old_index = atomic_load(&app->active_index);
    NativeSessionSlot *target_slot = &app->sessions[target];
    /* A cache-allocation fallback can intentionally reach here while a fresh worker is
     * still CONNECTING and before snapshot_pending is set, so this boundary is looser
     * than native_slot_is_live_stream_target; a terminal failure is never switchable. */
    if (!target_slot->rdp || atomic_load(&target_slot->session_failed)) {
        return;
    }
    if (old_index == target) {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
        native_preconnect_ui_select_slot(app->preconnect_ui, target);
#endif
        native_show_session_indicator(app, target);
        return;
    }
    clog(cLogLevelNotice, "switching video %s -> %s", native_session_slot_name(old_index),
         native_session_slot_name(target));
    /* Any switch that actually happens supersedes a deferred one still in flight. */
    app->pending_switch_target = -1;
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

    pthread_mutex_lock(&app->video_lock);
    /* The active flip and the stale-recovery clear live INSIDE video_lock: on_video_au
     * publishes video_refresh_needed under the same lock after re-checking the active
     * slot, so an outgoing worker mid-feed cannot interleave with the switch and leave
     * a stale request behind. A pending request belongs to the OUTGOING owner and goes
     * stale with the ownership change: draining it after the switch would send a
     * spurious refresh to the fresh target and arm its watchdog into a needless
     * reconnect on a static desktop. The old slot needs no recovery either way —
     * backgrounding reconnects it (snapshot) or suppresses it (resume asks for its own
     * keyframe on return). */
    unsigned target_epoch = atomic_load(&target_slot->connect_epoch);
    if (app->hub_return_rgba && app->hub_return_rgba_owner_slot == target) {
        if (!native_promote_hub_return_rgba_locked(app, target, target_epoch)) {
            /* The return slot reconnected while hidden. Its frozen pixels remain a
             * read-only fallback; the new generation gets a fresh mutable canvas. */
            native_close_rgba_locked(app, false);
            native_arm_hub_return_replacement_locked(
                app, target, target_epoch, atomic_load(&target_slot->video_ok_frames));
        }
    } else {
        native_close_rgba_locked(app, false);
        if (app->hub_return_rgba) {
            /* Choosing any other live stream commits the HUB exit; its original BACK
             * destination is no longer part of this handoff. */
            native_close_hub_return_rgba_locked(app);
        }
    }
    atomic_store(&app->active_index, target);
    atomic_store(&app->video_refresh_needed, false);
    app->video_plane_punched = false;
    pthread_mutex_unlock(&app->video_lock);
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    native_preconnect_ui_select_slot(app->preconnect_ui, target);
#endif
    native_duck_retarget(app);

    /* Input now drives the new session; the per-tick derive block re-arms the active
     * flag and re-syncs NumLock. */
    native_input_set_session(&app->input, target_slot->rdp);
    native_input_set_desktop_size(&app->input, (uint16_t)atomic_load(&target_slot->desktop_width),
                                  (uint16_t)atomic_load(&target_slot->desktop_height));
    app->input_locks_synced = false;
    native_request_pointer_window_size_update(app);
    native_cursor_reassert(&target_slot->cursor);

    /* Background the old server (graphics off, rdpsnd audio keeps feeding the mix; a
     * refresh-ineffective one reconnects hidden to cache its IDR) and wake the new one.
     * A previously suppressed session resumes with a delta frame the reloaded hardware
     * decoder cannot use, so bring the keyframe with us: a cached IDR snapshot replays
     * instantly, a refresh-capable server gets a keyframe request, and a refresh-
     * ineffective one without a snapshot reconnects for its connect IDR. */
    native_background_slot(app, old_index);
    if (target_slot->snapshot_pending) {
        /* The target's own hidden reconnect is still in flight: let the live connection
         * feed the decoder directly — its first frame IS the connect IDR (make-before-
         * break). In the ms-wide window where that IDR already landed in the cache
         * instead, the keyframe watchdog below reconnects — no worse than before. */
        target_slot->snapshot_pending = false;
        pthread_mutex_lock(&app->video_lock);
        native_au_snapshot_reset(&target_slot->snapshot);
        pthread_mutex_unlock(&app->video_lock);
        atomic_store(&target_slot->snapshot_idr_ready, false);
    } else if (target_slot->suppressed) {
        target_slot->suppressed = false;
        if (native_snapshot_replay(app, target)) {
            /* Decoder state rebuilt from the cached connect IDR: resume plainly — the
             * server's next delta continues that exact chain. No refresh, no reconnect. */
            rdp_set_suppress_output(target_slot->rdp, true);
        } else if (atomic_load(&target_slot->session_failed)) {
            /* The replay attempt flagged the slot (terminal feed error): the failure
             * drain routes it; nothing to resume here. */
        } else if (target_slot->refresh_ineffective && !NATIVE_SWITCH_TEST_NO_RECONNECT) {
            /* This server never delivers a keyframe on request (no Display Control
             * channel, Refresh Rect ignored — learned from an earlier watchdog timeout).
             * Skip the doomed wait and reconnect right away: a fresh connection always
             * starts with an IDR. */
            clog(cLogLevelWarning,
                 "%s server yields no keyframe on request; reconnecting immediately for a fresh IDR",
                 native_session_slot_name(target));
            if (!native_slot_connect(app, target, false)) {
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

/* Starts filling `target`'s AU snapshot so a deferred switch can complete without a
 * black reload window: the slot stays in the BACKGROUND (the current stream keeps the
 * screen) while a keyframe request — or, for a refresh-ineffective server, a hidden
 * reconnect — produces the IDR into the armed cache. The switch tick completes the
 * switch by replay once the cache is ready and the stream has gone quiet again. */
static void native_prepare_pending_switch(App *app, int target) {
    NativeSessionSlot *slot = &app->sessions[target];
    if (slot->snapshot_pending) {
        return; /* a hidden reconnect is already filling the cache */
    }
    bool reconnect = (slot->refresh_ineffective || app->snapshot_force) && !NATIVE_SWITCH_TEST_NO_RECONNECT;
    bool armed;
    if (reconnect) {
        clog(cLogLevelNotice,
             "reconnecting the %s session in the background for the deferred switch",
             native_session_slot_name(target));
        /* One reconnect per deferred switch: a stream that produces no AU even on a
         * fresh connection (bitmap path) must not loop reconnects forever. */
        slot->snapshot_retry_used = true;
        /* Arming happens inside the connect (old worker stopped, new not yet started),
         * so the connect IDR cannot race an unarmed cache. */
        if (!native_slot_connect(app, target, true)) {
            /* Same failure routing as every connect path; the tick cancels the switch. */
            slot_stop_with_state(slot, RDP_STATE_NETWORK_ERROR, rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
            return;
        }
        pthread_mutex_lock(&app->video_lock);
        armed = slot->snapshot.armed;
        pthread_mutex_unlock(&app->video_lock);
    } else {
        /* No reconnect: the suppressed connection emits nothing until the resume below,
         * which is sent only after the cache is armed. */
        atomic_store(&slot->snapshot_idr_ready, false);
        pthread_mutex_lock(&app->video_lock);
        armed = native_au_snapshot_arm(&slot->snapshot);
        pthread_mutex_unlock(&app->video_lock);
    }
    if (!armed) {
        /* No memory for the cache: fall back to the immediate old-style switch (its
         * splash covers the reload) rather than leaving the press unanswered. */
        clog(cLogLevelWarning, "%s AU snapshot allocation failed; switching immediately",
             native_session_slot_name(target));
        app->pending_switch_target = -1;
        native_complete_session_switch(app, target);
        return;
    }
    slot->snapshot_pending = true; /* the tick suppresses once the IDR lands */
    slot->snapshot_deadline_ticks = 0;
    slot->suppressed = false;
    if (!reconnect) {
        /* Resume output and ask for a keyframe; the IDR lands in the cache because the
         * slot stays backgrounded. A server that yields none hits the no-AU deadline,
         * which learns refresh_ineffective and reconnects — still behind the live
         * stream. */
        rdp_set_suppress_output(slot->rdp, true);
        rdp_request_refresh(slot->rdp);
    }
}

/* Reveal the four-card HUB without stopping the active RDP session. Translucent LVGL
 * chrome is composited over the still-running video plane, while input is released so
 * the remote can navigate locally. A subsequent card action resumes/switches through
 * the normal session path. */
static void native_show_hub(App *app) {
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    if (!app || !app->streaming_visible || app->mixer_overlay_visible || !app->preconnect_ui) {
        return;
    }
    int active = atomic_load(&app->active_index);
    clog(cLogLevelInfo, "central remote button opens HUB on the %s profile",
         native_session_slot_name(active));

    /* A HUB request supersedes a deferred colour-key switch. Its target can finish
     * snapshot backgrounding normally; it no longer owns navigation. */
    app->pending_switch_target = -1;
    native_stop_streaming_input(app);
    native_input_set_active(&app->input, false);
#if HELLOLG_WITH_EVDEV_INPUT
    native_preconnect_ui_set_keyboard_available(app->preconnect_ui, native_evdev_input_probe_keyboard());
    native_preconnect_ui_set_mouse_available(app->preconnect_ui, native_evdev_input_probe_mouse());
#endif
    native_cursor_show_default();
    app->indicator_slot = -1;
    app->indicator_drawn = false;
    app->switch_splash_drawn = false;
    pthread_mutex_lock(&app->video_lock);
    /* Re-arm the one-shot punch for the eventual return so the final HUB chrome frame
     * is cleared before raw streaming input resumes. */
    app->video_plane_punched = false;
    pthread_mutex_unlock(&app->video_lock);
    app->streaming_visible = false;
    app->hub_visible = true;
    app->hub_return_slot = active;
    app->hub_connect_target = -1;
    app->ui_last_state = -1;
    native_preconnect_ui_show_hub(app->preconnect_ui, active);
    native_preconnect_ui_set_visible(app->preconnect_ui, true);
#else
    (void)app;
#endif
}

/* Navigates the screen to `target`'s configurator (pre-connect form) while any current
 * session keeps running in the background: its graphics are suppressed server-side and
 * its audio keeps feeding the mix. */
static void native_show_slot_configurator(App *app, int target) {
    /* Navigating to a form invalidates a deferred switch; the prepared slot finishes
     * backgrounding on its own. */
    app->pending_switch_target = -1;
    app->hub_connect_target = -1;
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    int old_index = atomic_load(&app->active_index);
    if (old_index != target) {
        clog(cLogLevelInfo, "showing the %s session configurator", native_session_slot_name(target));
        /* Releases owed to the old server must go out while input is still wired to it;
         * the evdev grab is dropped because the configurator needs the SDL mouse. */
        native_stop_streaming_input(app);
#if HELLOLG_WITH_EVDEV_INPUT
        native_preconnect_ui_set_keyboard_available(app->preconnect_ui, native_evdev_input_probe_keyboard());
        native_preconnect_ui_set_mouse_available(app->preconnect_ui, native_evdev_input_probe_mouse());
#endif
        native_input_set_active(&app->input, false);
        pthread_mutex_lock(&app->video_lock);
        /* Capture before the ownership flip: an in-flight old bitmap callback either
         * finishes under this lock and is frozen with the frame, or observes the new
         * active slot on its under-lock recheck and drops the update. */
        native_capture_hub_return_rgba_locked(app);
        native_close_rgba_locked(app, false);
        atomic_store(&app->active_index, target);
        /* The video track stays open behind the (opaque) configurator: closing it would
         * reload the shared pipeline and cut the mixed audio for nothing. Returning to a
         * stream swaps the decoder on that stream's next keyframe (on_video_au). */
        app->video_plane_punched = false;
        pthread_mutex_unlock(&app->video_lock);
        native_duck_retarget(app);
        /* Demote the old slot BEFORE backgrounding it: snapshot backgrounding reconnects
         * the slot, and native_slot_connect must see it as inactive so the shared
         * decoder/input state stays with the incoming screen. */
        native_background_slot(app, old_index);
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
        native_preconnect_ui_set_connecting(app->preconnect_ui, old_index, false, "");
        native_preconnect_ui_open_setup(app->preconnect_ui, target);
        native_preconnect_ui_set_visible(app->preconnect_ui, true);
    }
#else
    (void)app;
    (void)target;
    clog(cLogLevelWarning, "no pre-connect UI in this build; cannot open a session configurator");
#endif
}

/* Color-button press: every button is "go to that computer" — resume a live stream,
 * connect a configured offline profile, or open setup for an empty slot. */
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
    if (target == atomic_load(&app->active_index) && app->pending_switch_target >= 0) {
        /* The active slot's key while a deferred switch is in flight = change of mind:
         * stay here. The prepared slot finishes backgrounding on its own (the tick
         * suppresses it once its IDR lands), ready for a later switch. */
        clog(cLogLevelInfo, "deferred switch to %s canceled",
             native_session_slot_name(app->pending_switch_target));
        app->pending_switch_target = -1;
        native_show_session_indicator(app, target);
        return;
    }
    /* A slot mid-hidden-reconnect (snapshot_pending) is still a live stream target: the
     * user watched it moments ago; treat the press as a switch rather than dropping to
     * its configurator. */
    if (native_slot_is_live_stream_target(slot)) {
        /* A live resume/switch answers the HUB visit. Do this only after identifying a
         * stream-capable target: choosing a still-CONNECTING background card must leave
         * HUB open until that connection can actually be entered. */
        app->hub_visible = false;
        app->hub_connect_target = -1;
        if (target == atomic_load(&app->active_index) && app->streaming_visible) {
            /* Re-pressing the active slot's button is no switch — open the live volume
             * mixer instead of just re-flashing the badge. */
            native_mixer_overlay_show(app);
            return;
        }
        if (target == app->pending_switch_target) {
            return; /* already preparing this switch; the tick completes it */
        }
        /* Switch NOW only when the target can take the screen without a black reload
         * window: it is streaming live (never suppressed), or its cached IDR is ready
         * and the stream has been quiet past the suppress tail. Anything else defers:
         * the CURRENT stream keeps the screen while the target gets ready. */
        bool ready_now;
        if (atomic_load(&slot->video_via_bitmap)) {
            /* Bitmap streams have no AUs to snapshot; resume simply restarts bitmap
             * updates, so the old immediate switch IS the right path for them. */
            ready_now = true;
        } else if (slot->snapshot_pending) {
            ready_now = false; /* reconnect still filling the cache */
        } else if (!slot->suppressed) {
            ready_now = true; /* live stream (including the active slot itself) */
        } else {
            ready_now = atomic_load(&slot->snapshot_idr_ready) &&
                        native_monotonic_ms() - atomic_load(&slot->snapshot_last_au_ms) >=
                            NATIVE_SNAPSHOT_QUIET_MS;
        }
        if (ready_now) {
            app->pending_switch_target = -1;
            native_complete_session_switch(app, target);
            return;
        }
        clog(cLogLevelInfo, "deferring the switch to %s until its stream is ready",
             native_session_slot_name(target));
        app->pending_switch_target = target;
        app->pending_switch_deadline_ticks = SDL_GetTicks() + NATIVE_PENDING_SWITCH_TIMEOUT_MS;
        slot->snapshot_retry_used = false;
        /* Acknowledge the press right away: the badge shows the DESTINATION while the
         * current stream keeps playing. */
        native_show_session_indicator(app, target);
        if (slot->suppressed && atomic_load(&slot->snapshot_idr_ready)) {
            /* The cache is ready and merely inside the quiet window (a fast flip-back):
             * preparing would throw it away for a needless reconnect. Just wait — the
             * tick completes the switch within NATIVE_SNAPSHOT_QUIET_MS. */
            return;
        }
        native_prepare_pending_switch(app, target);
        return;
    }
    if (slot->rdp) {
        /* A worker that has not reached ACTIVE is already connecting. Do not queue a
         * duplicate attempt; its live card state will catch up on the next loop tick. */
        return;
    }
    native_show_slot_configurator(app, target);
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    (void)native_preconnect_ui_request_connect(app->preconnect_ui, target);
#endif
}

/* Remote D-pad ring switching: left/right cycles to the neighbouring stream-capable slot
 * (red -> green -> yellow -> blue -> red). Only slots holding a session that can take
 * the screen participate — empty slots stay reachable through their color buttons — and
 * with nothing else connected the press just re-flashes the badge. */
static void native_ring_switch(App *app, int direction) {
    int active = atomic_load(&app->active_index);
    for (int step = 1; step < NATIVE_SETTINGS_MAX_SESSIONS; step++) {
        int candidate = (active + direction * step) % NATIVE_SETTINGS_MAX_SESSIONS;
        if (candidate < 0) {
            candidate += NATIVE_SETTINGS_MAX_SESSIONS;
        }
        NativeSessionSlot *slot = &app->sessions[candidate];
        if (native_slot_is_live_stream_target(slot)) {
            native_request_session_switch(app, candidate);
            return;
        }
    }
    native_show_session_indicator(app, active); /* alone on the ring: acknowledge the press */
}

/* Per-tick bookkeeping: worker-side refresh requests, snapshot backgrounding and the
 * keyframe watchdog. */
static void native_switch_tick(App *app) {
    /* Snapshot backgrounding: once a hidden-reconnected slot has its connect IDR cached,
     * silence its server. Nothing further is transmitted after the cached AUs, so the
     * post-resume delta will reference exactly the state a replay rebuilds. A snapshot
     * voided before this tick could suppress (an oversize IDR overflowed the cache) is
     * suppressed too — otherwise the backgrounded session would stream into the drop
     * path indefinitely; with no cache the switch-back takes the reconnect fallback. */
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        NativeSessionSlot *slot = &app->sessions[i];
        if (!slot->snapshot_pending || i == atomic_load(&app->active_index)) {
            continue;
        }
        if (!slot->rdp || atomic_load(&slot->current_state) != (int)RDP_STATE_ACTIVE) {
            continue;
        }
        bool idr_ready = atomic_load(&slot->snapshot_idr_ready);
        bool voided = false;
        if (!idr_ready) {
            /* pending implies the snapshot was armed; disarmed now = append voided it. */
            pthread_mutex_lock(&app->video_lock);
            voided = !slot->snapshot.armed;
            pthread_mutex_unlock(&app->video_lock);
        }
        if (idr_ready || voided) {
            rdp_set_suppress_output(slot->rdp, false);
            slot->suppressed = true;
            slot->snapshot_pending = false;
            if (idr_ready) {
                clog(cLogLevelDebug, "%s connect IDR cached; suppressing the backgrounded session",
                     native_session_slot_name(i));
            } else {
                clog(cLogLevelWarning,
                     "%s snapshot voided; suppressing the backgrounded session without a cache",
                     native_session_slot_name(i));
                if (i == app->pending_switch_target) {
                    /* The deferred switch lost its cache (oversize IDR): answer the
                     * press with the immediate old-style switch rather than never. */
                    app->pending_switch_target = -1;
                    native_complete_session_switch(app, i);
                    continue;
                }
            }
        } else if (slot->snapshot_deadline_ticks == 0) {
            /* Count from ACTIVE (the guard above), not from the reconnect: a slow
             * TLS/CredSSP handshake must not eat the whole window. */
            slot->snapshot_deadline_ticks = SDL_GetTicks() + NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
        } else if (SDL_TICKS_PASSED(SDL_GetTicks(), slot->snapshot_deadline_ticks)) {
            if (i == app->pending_switch_target && !NATIVE_SWITCH_TEST_NO_RECONNECT &&
                !slot->snapshot_retry_used && !atomic_load(&slot->video_via_bitmap)) {
                /* A deferred switch asked this server for a keyframe and none came:
                 * learn it and fall to the hidden reconnect — the user is still watching
                 * the live current stream, so the lesson costs no black screen. */
                clog(cLogLevelWarning,
                     "%s yielded no keyframe within %ums; reconnecting in the background (deferred switch)",
                     native_session_slot_name(i), (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
                slot->refresh_ineffective = true;
                slot->snapshot_pending = false;
                native_prepare_pending_switch(app, i);
                continue;
            }
            /* No AU ever seeded the snapshot — e.g. the stream negotiated the RemoteFX
             * bitmap path, which feeds on_bitmap_update instead of on_video_au. Silence
             * the server anyway; the cacheless switch-back takes the reconnect fallback. */
            pthread_mutex_lock(&app->video_lock);
            native_au_snapshot_reset(&slot->snapshot);
            pthread_mutex_unlock(&app->video_lock);
            rdp_set_suppress_output(slot->rdp, false);
            slot->suppressed = true;
            slot->snapshot_pending = false;
            clog(cLogLevelWarning,
                 "%s snapshot got no AUs within %ums; suppressing the backgrounded session without a cache",
                 native_session_slot_name(i), (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS);
            if (i == app->pending_switch_target) {
                /* Reached with the one retry spent (a stream that never feeds
                 * on_video_au, e.g. the bitmap path) or in no-reconnect test builds:
                 * answer the press with the immediate old-style switch. */
                app->pending_switch_target = -1;
                native_complete_session_switch(app, i);
                continue;
            }
        }
    }

    /* Deferred switch: complete once the target's cache is ready and its stream has
     * been quiet past the suppress tail — the replay is then race-free and instant. */
    if (app->pending_switch_target >= 0) {
        int target = app->pending_switch_target;
        NativeSessionSlot *slot = &app->sessions[target];
        if (!slot->rdp || atomic_load(&slot->session_failed)) {
            /* The prepared session died; the failure drain reports it. Stay put. */
            clog(cLogLevelWarning, "deferred switch to %s abandoned (session gone)",
                 native_session_slot_name(target));
            app->pending_switch_target = -1;
        } else if (!slot->snapshot_pending && slot->suppressed && atomic_load(&slot->snapshot_idr_ready) &&
                   native_monotonic_ms() - atomic_load(&slot->snapshot_last_au_ms) >= NATIVE_SNAPSHOT_QUIET_MS) {
            clog(cLogLevelDebug, "%s stream is ready; completing the deferred switch",
                 native_session_slot_name(target));
            app->pending_switch_target = -1;
            native_complete_session_switch(app, target);
        } else if (SDL_TICKS_PASSED(SDL_GetTicks(), app->pending_switch_deadline_ticks)) {
            /* The target never became cleanly ready — it keeps streaming through the
             * suppress (quiet window never opens) or its cache keeps dying. Answer the
             * press with the immediate old-style switch instead of staying pending
             * forever with further presses ignored. */
            clog(cLogLevelWarning, "deferred switch to %s timed out after %ums; switching the old way",
                 native_session_slot_name(target), (unsigned)NATIVE_PENDING_SWITCH_TIMEOUT_MS);
            app->pending_switch_target = -1;
            native_complete_session_switch(app, target);
        }
    }

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
                clog(cLogLevelDebug,
                     "TEST: watchdog expired; would reconnect the %s session, keeping the connection",
                     native_session_slot_name(active->index));
                app->switch_deadline_ticks = SDL_GetTicks() + 10000u; /* re-check, keep logging */
            } else if (!app->switch_reconnect_used && active->rdp &&
                atomic_load(&active->current_state) == (int)RDP_STATE_ACTIVE) {
                clog(cLogLevelWarning,
                     "no keyframe %ums after the switch; reconnecting the %s session for a fresh IDR",
                     (unsigned)NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS,
                     native_session_slot_name(active->index));
                /* Remember: switches to this slot should reconnect immediately from now on. */
                active->refresh_ineffective = true;
                app->switch_reconnect_used = true;
                app->switch_deadline_ticks = SDL_GetTicks() + 2u * NATIVE_SWITCH_KEYFRAME_TIMEOUT_MS;
                app->switch_baseline_frames = atomic_load(&active->video_ok_frames);
                if (!native_slot_connect(app, active->index, false)) {
                    /* Same failure routing as the fast-reconnect path above. */
                    slot_stop_with_state(active, RDP_STATE_NETWORK_ERROR,
                                         rdp_state_exit_code(RDP_STATE_NETWORK_ERROR));
                }
            } else {
                clog(cLogLevelError, "switch watchdog gave up waiting for video frames");
                app->switch_deadline_ticks = 0;
            }
        }
    }
}


static void handle_sdl_event(App *app, SDL_Window *window, SDL_Renderer *renderer, const SDL_Event *event) {
    switch (event->type) {
    case SDL_QUIT:
        clog(cLogLevelNotice, "SDL_QUIT requests shutdown");
        atomic_store(&app->running, false);
        break;
    case SDL_APP_TERMINATING:
        clog(cLogLevelNotice, "SDL_APP_TERMINATING requests shutdown");
        atomic_store(&app->running, false);
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        clog(cLogLevelInfo, "SDL_APP_WILLENTERBACKGROUND; keeping native session running");
        native_stop_streaming_input(app);
        break;
    case SDL_APP_DIDENTERBACKGROUND:
        clog(cLogLevelInfo, "SDL_APP_DIDENTERBACKGROUND; keeping native session running");
        native_stop_streaming_input(app); /* belt and suspenders */
        break;
    case SDL_APP_WILLENTERFOREGROUND:
        clog(cLogLevelDebug, "SDL_APP_WILLENTERFOREGROUND");
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        clog(cLogLevelInfo, "SDL_APP_DIDENTERFOREGROUND");
        app->window_unfocused = false;
        native_resume_streaming_input(app, window);
        break;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
            clog(cLogLevelNotice, "SDL window close requests shutdown");
            atomic_store(&app->running, false);
        } else if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event->window.event == SDL_WINDOWEVENT_RESIZED) {
            clog(cLogLevelDebug, "SDL window size event %u: %dx%d", (unsigned)event->window.event,
                 event->window.data1, event->window.data2);
            (void)native_update_render_size(app, renderer);
            native_update_pointer_window_size(app);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST || event->window.event == SDL_WINDOWEVENT_HIDDEN ||
                   event->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            /* A remote-invoked webOS overlay (TV menu, notifications) steals input focus WITHOUT
             * fully backgrounding the app, so SDL_APP_WILLENTERBACKGROUND never fires. Release
             * the global evdev grab here too, otherwise the mouse/keyboard stay locked to us and
             * are unusable in the overlay. Re-grab happens on FOCUS_GAINED below. */
            clog(cLogLevelInfo, "window lost focus (event %u); releasing input grab",
                 (unsigned)event->window.event);
            app->window_unfocused = true;
            if (app->mixer_overlay_visible && app->mixer_overlay_dismiss_button != 0) {
                /* Focus loss can swallow the matching SDL button-up. It is safe to
                 * finish the deferred close now because input remains ungrabbed until
                 * focus returns. */
                app->mixer_overlay_dismiss_button = 0;
                native_mixer_overlay_hide(app);
            }
            native_stop_streaming_input(app);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event->window.event == SDL_WINDOWEVENT_RESTORED ||
                   event->window.event == SDL_WINDOWEVENT_SHOWN) {
            clog(cLogLevelInfo, "window gained focus (event %u); re-grabbing input",
                 (unsigned)event->window.event);
            app->window_unfocused = false;
            native_resume_streaming_input(app, window);
        } else if (event->window.event == SDL_WINDOWEVENT_EXPOSED) {
            clog(cLogLevelTrace, "SDL window lifecycle event %u", (unsigned)event->window.event);
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
                if (app->mixer_overlay_selected == NATIVE_UI_MIXER_MASTER) {
                    native_mixer_overlay_set_master_pct(app, native_ui_mixer_fader_pct_at(win_h, event->motion.y));
                } else {
                    native_mixer_overlay_set_db(app, app->mixer_overlay_selected,
                                                native_ui_mixer_fader_db_at(win_h, event->motion.y));
                }
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
                if (app->mixer_overlay_dismiss_button == event->button.button) {
                    app->mixer_overlay_dismiss_button = 0;
                    native_mixer_overlay_hide(app);
                    break;
                }
                native_mixer_overlay_touch(app);
                break;
            }
            int win_w = 0;
            int win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            int hit_slot = -1;
            NativeUiMixerHit zone = NATIVE_UI_MIXER_HIT_BODY;
            if (!native_ui_mixer_hit_test(win_w, win_h, event->button.x, event->button.y, &hit_slot, &zone)) {
                /* Keep the overlay alive until this same button is released. Closing
                 * on DOWN would immediately restore the grab and could leak the UP edge
                 * into the RDP session as a stray pointer release. */
                if (app->mixer_overlay_dismiss_button == 0) {
                    app->mixer_overlay_dismiss_button = event->button.button;
                }
                app->mixer_overlay_dragging = false;
                native_mixer_overlay_touch(app);
                break;
            }
            if (event->button.button != SDL_BUTTON_LEFT) {
                native_mixer_overlay_touch(app);
                break;
            }
            if (hit_slot >= 0) {
                native_mixer_overlay_select(app, hit_slot);
                if (zone == NATIVE_UI_MIXER_HIT_MUTE && hit_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                    native_mixer_overlay_toggle_mute(app, hit_slot);
                } else if (zone == NATIVE_UI_MIXER_HIT_SOLO && hit_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                    native_mixer_overlay_toggle_solo(app, hit_slot);
                } else if (zone == NATIVE_UI_MIXER_HIT_DUCK && hit_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                    /* A click on the switch toggles it (inert for the active channel);
                     * no drag state — the switch has no fader travel. */
                    native_mixer_overlay_toggle_duck(app, hit_slot);
                } else if (zone == NATIVE_UI_MIXER_HIT_FADER) {
                    if (hit_slot == NATIVE_UI_MIXER_MASTER) {
                        native_mixer_overlay_set_master_pct(app,
                                                            native_ui_mixer_fader_pct_at(win_h, event->button.y));
                    } else {
                        native_mixer_overlay_set_db(app, hit_slot,
                                                    native_ui_mixer_fader_db_at(win_h, event->button.y));
                    }
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
         * keyboard path. SDL keys here belong to the ungrabbed webOS remote: its colour
         * keys navigate sessions, central OK opens HUB, and the channel rocker rings
         * connected slots. App exit is system-driven (webOS EXIT/home -> SDL_QUIT /
         * SDL_APP_TERMINATING above).
         *
         * The mouse, by contrast, keeps an SDL fallback above: a grabbed USB mouse is read via
         * evdev, but with no USB mouse SDL still delivers the compositor pointer (Magic Remote). */
#if !defined(HELLOLG_WITH_PRECONNECT_UI) || !HELLOLG_WITH_PRECONNECT_UI
        if (app->streaming_visible && !app->mixer_overlay_visible &&
            native_sdl_confirm_key(&event->key)) {
            /* Without HUB support the fallback remote's OK button remains an ordinary
             * RDP Enter. Forward both edges; dropping the release can leave Enter held
             * on the server. SDL repeat does not represent a new physical edge. */
            if (event->type == SDL_KEYDOWN && event->key.repeat != 0) {
                break;
            }
            bool down = event->type == SDL_KEYDOWN;
            if (native_input_key(&app->input, 0x1c, down, false)) {
                native_track_key(app, 0x1c, false, down);
            }
            break;
        }
#endif
        if (event->type == SDL_KEYDOWN) {
            /* Only the ungrabbed remote (and system) reaches SDL here — the USB keyboard
             * is evdev-grabbed — so these are sparse; logging them maps what a given
             * remote firmware actually sends. */
            clog(cLogLevelTrace, "remote sdl key scancode=%d", (int)event->key.keysym.scancode);
            int slot = native_sdl_webos_color_slot(&event->key);
            if (slot >= 0) {
                native_request_session_switch(app, slot);
            } else if (app->mixer_overlay_visible) {
                native_mixer_overlay_sdl_key(app, &event->key);
            } else if (app->streaming_visible && event->key.repeat == 0 &&
                       native_sdl_confirm_key(&event->key)) {
                /* SDL is the fallback for remote nodes the evdev grab does not own.
                 * A single central-button down opens HUB; its release is ignored. */
                native_show_hub(app);
            } else if (event->key.keysym.scancode == 480 /* SDL_WEBOS_SCANCODE_CH_UP */ ||
                       event->key.keysym.scancode == 481 /* SDL_WEBOS_SCANCODE_CH_DOWN */) {
                /* The channel rocker rings through the connected slots — channel-zapping
                 * semantics. It has no meaning inside an RDP session, so nothing is
                 * taken away from the remote desktop. */
                native_ring_switch(app, event->key.keysym.scancode == 480 ? 1 : -1);
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
        clog(cLogLevelError, "SDL_InitSubSystem(VIDEO|EVENTS) failed: %s", SDL_GetError());
        return 4;
    }
    native_log_sdl_display_modes();

    clog(cLogLevelInfo, "creating borderless SDL window %dx%d", NATIVE_LOCAL_SURFACE_WIDTH,
         NATIVE_LOCAL_SURFACE_HEIGHT);
    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS;
    SDL_Window *window = SDL_CreateWindow("gnomecast", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          NATIVE_LOCAL_SURFACE_WIDTH, NATIVE_LOCAL_SURFACE_HEIGHT, window_flags);
    if (!window) {
        clog(cLogLevelError, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
    /* Cursor visibility is event-driven from here on: the preconnect UI keeps the default
     * arrow, and during a session the server's pointer updates drive shape/visibility
     * through native_cursor_apply (cursor_sdl.c). */

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
        clog(cLogLevelError, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
#else
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        clog(cLogLevelError, "SDL_CreateRenderer failed: %s", SDL_GetError());
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
        clog(cLogLevelError, "%s", surface_message);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
    SDL_RaiseWindow(window);
    SDL_StartTextInput();

    uint16_t loop_fps = settings->sessions[NATIVE_SESSION_SLOT_GREEN].fps;
    clog(cLogLevelInfo, "SDL loop running at target %u fps", (unsigned)loop_fps);

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
    NativePreconnectUi *ui = native_preconnect_ui_create(window, renderer, settings->sessions,
                                                         settings->audio_codec);
    if (!ui) {
        clog(cLogLevelError, "failed to create pre-connect UI");
        SDL_StopTextInput();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 4;
    }
    native_preconnect_ui_set_background_drawer(ui, native_draw_preconnect_background, app);
    app->preconnect_ui = ui;
    int initial_hub_slot = native_preconnect_ui_selected_slot(ui);
    if (initial_hub_slot >= 0 && initial_hub_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        atomic_store(&app->active_index, initial_hub_slot);
        native_duck_retarget(app);
    }

    app->interactive_ui = true;
    app->ui_last_state = -1;
    app->streaming_visible = false;
    app->hub_visible = false;
    bool present_logged = false;

#if HELLOLG_WITH_EVDEV_INPUT
    /* The hub mirrors the current USB input probes in its compact icon indicators. */
    native_preconnect_ui_set_keyboard_available(ui, native_evdev_input_probe_keyboard());
    native_preconnect_ui_set_mouse_available(ui, native_evdev_input_probe_mouse());
#endif

    while (atomic_load(&app->running)) {
        NativeSessionSlot *active = native_active_slot(app);
        int event_state = atomic_load(&active->current_state);
        if (app->streaming_visible && active->rdp && event_state == (int)RDP_STATE_ACTIVE) {
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
        native_system_volume_tick(app);

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
            native_preconnect_ui_set_slot_state(ui, i, NATIVE_PRECONNECT_SESSION_ERROR, status);
            if (i == app->hub_connect_target) {
                /* A HUB-originated connect failed before it could own the screen. Keep
                 * HUB open and returnable, but stop waiting for this slot to auto-enter. */
                app->hub_connect_target = -1;
            }

            if (i == atomic_load(&app->active_index)) {
                int survivor = -1;
                for (int j = 0; j < NATIVE_SETTINGS_MAX_SESSIONS; j++) {
                    if (j != i && native_slot_is_live_stream_target(&app->sessions[j]) &&
                        atomic_load(&app->sessions[j].current_state) == (int)RDP_STATE_ACTIVE) {
                        survivor = j;
                    }
                }
                if (survivor >= 0 && app->streaming_visible) {
                    /* The session the user was WATCHING died: fall back to the survivor. */
                    clog(cLogLevelWarning, "%s; auto-switching video to the %s session", status,
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
                    native_preconnect_ui_set_connecting(ui, i, false, status);
                    native_preconnect_ui_set_status(ui, status, true);
                } else {
                    /* Flush + release input while the failed session pointer is still
                     * wired, then tear the slot down (media follows after the pass once
                     * no slot needs it). */
                    native_stop_streaming_input(app);
#if HELLOLG_WITH_EVDEV_INPUT
                    native_preconnect_ui_set_keyboard_available(ui, native_evdev_input_probe_keyboard());
                    native_preconnect_ui_set_mouse_available(ui, native_evdev_input_probe_mouse());
#endif
                    native_stop_slot(app, i);
                    native_cursor_reset(&slot->cursor);
                    app->switch_deadline_ticks = 0;
                    app->streaming_visible = false;
                    native_mixer_overlay_force_hide(app);
                    app->ui_last_state = -1;
                    native_preconnect_ui_set_visible(ui, true);
                    native_preconnect_ui_set_connecting(ui, i, false, status);
                    native_preconnect_ui_set_status(ui, status, true);
                }
            } else {
                clog(cLogLevelWarning, "background %s", status);
                native_stop_slot(app, i);
            }
            if (!app->streaming_visible && !app->hub_visible) {
                int pending_return = app->pending_switch_target;
                bool pending_return_live = false;
                if (pending_return >= 0 && pending_return < NATIVE_SETTINGS_MAX_SESSIONS) {
                    NativeSessionSlot *pending_slot = &app->sessions[pending_return];
                    pending_return_live = native_slot_is_live_stream_target(pending_slot);
                }
                if (!pending_return_live) {
                    /* A HUB return can be accepted just before its target dies. The UI
                     * is still on screen in that race, so make it interactive again
                     * instead of waiting for a hide transition that will never happen. */
                    native_preconnect_ui_cancel_hub_close(ui);
                }
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

        /* Keep every card live, including sessions that are connected in the background.
         * Terminal failures above own the ERROR state after their worker is drained. */
        uint64_t runtime_now = native_monotonic_ms64();
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            NativeSessionSlot *slot = &app->sessions[i];
            bool failed = atomic_load(&slot->session_failed);
            int slot_state = atomic_load(&slot->current_state);
            bool session_exists = slot->rdp && !failed;
            if (!session_exists) {
                app->session_started_ms[i] = 0;
                app->session_runtime_active[i] = false;
                native_preconnect_ui_set_slot_runtime(ui, i, 0, 0, 0, false, 0, 0, 0, 0, 0);
            } else if (slot_state == (int)RDP_STATE_ACTIVE || app->session_runtime_active[i]) {
                /* Start only once the user's connection reaches ACTIVE. Hidden snapshot
                 * and watchdog reconnects retain the latch and original start stamp. */
                if (!app->session_runtime_active[i]) {
                    app->session_started_ms[i] = runtime_now;
                    app->session_runtime_active[i] = true;
                }
                uint64_t session_minutes64 = (runtime_now - app->session_started_ms[i]) / 60000u;
                uint32_t session_minutes =
                    session_minutes64 > UINT32_MAX ? UINT32_MAX : (uint32_t)session_minutes64;
                NativeAudioSourceStats audio_stats;
                bool audio_stream_open =
                    native_audio_pipeline_get_source_stats(&app->audio_pipeline, i, &audio_stats) && audio_stats.open;
                uint32_t audio_codec = audio_stream_open ? atomic_load(&slot->audio_codec) : 0u;
                uint32_t audio_sample_rate = audio_stream_open ? atomic_load(&slot->audio_sample_rate) : 0u;
                uint16_t audio_channels =
                    audio_stream_open ? (uint16_t)atomic_load(&slot->audio_channels) : 0u;
                int32_t audio_peak_left = 0;
                int32_t audio_peak_right = 0;
                native_audio_pipeline_get_source_peaks(&app->audio_pipeline, i, &audio_peak_left,
                                                       &audio_peak_right);
                native_preconnect_ui_set_slot_runtime(
                    ui, i, (uint16_t)atomic_load(&slot->desktop_width),
                    (uint16_t)atomic_load(&slot->desktop_height),
                    session_minutes, audio_stream_open, audio_codec, audio_sample_rate, audio_channels,
                    audio_peak_left, audio_peak_right);
            } else {
                /* A fresh connection is still negotiating, so no session clock exists
                 * yet. Its card state below remains CONNECTING. */
                native_preconnect_ui_set_slot_runtime(ui, i, 0, 0, 0, false, 0, 0, 0, 0, 0);
            }
            if (!slot->rdp || failed) {
                continue;
            }
            native_preconnect_ui_set_slot_state(
                ui, i,
                slot_state == (int)RDP_STATE_ACTIVE ? NATIVE_PRECONNECT_SESSION_CONNECTED
                                                    : NATIVE_PRECONNECT_SESSION_CONNECTING,
                slot_state == (int)RDP_STATE_ACTIVE ? NULL : rdp_state_name((RdpState)slot_state));
        }

        active = native_active_slot(app);
        int state = atomic_load(&active->current_state);
        /* Input arming is derived here, on the SDL thread, from the active slot's state:
         * worker callbacks must not write shared input state (see on_state). */
        /* The mixer overlay owns the pointer while visible (evdev grab released, system
         * cursor shown): keep RDP input disarmed so nothing leaks to the session. */
        bool input_streaming = app->streaming_visible && active->rdp && state == (int)RDP_STATE_ACTIVE &&
                               !app->mixer_overlay_visible;
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
        bool hub_connect_ready = app->hub_connect_target == atomic_load(&app->active_index);
        if (active->rdp && state == (int)RDP_STATE_ACTIVE && !app->streaming_visible &&
            app->pending_switch_target < 0 &&
            (!app->hub_visible || hub_connect_ready)) {
            if (hub_connect_ready) {
                /* Only a successful ACTIVE transition commits the HUB exit. Until this
                 * point BACK must still be able to resume the stream below the HUB. */
                app->hub_connect_target = -1;
                app->hub_visible = false;
            }
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
            native_preconnect_ui_set_connecting(ui, atomic_load(&app->active_index), false, "");
            native_preconnect_ui_set_visible(ui, false);
            app->window_unfocused = false; /* enter streaming focused, whatever fired during preconnect */
            /* Grab input now that streaming has started; the preconnect UI needs the SDL mouse,
             * so we don't grab earlier. Note any degradation on the (now hidden) preconnect
             * status so it shows if the session returns here; the loud log covers diagnosis. */
            NativeInputStartResult input_result = native_start_streaming_input(app);
#if HELLOLG_WITH_EVDEV_INPUT
            native_preconnect_ui_set_mouse_available(ui, native_evdev_input_mouse_active(&app->evdev_input));
#endif
            switch (input_result) {
            case NATIVE_INPUT_START_OK:
                native_preconnect_ui_set_keyboard_available(ui, true);
                break;
            case NATIVE_INPUT_START_NO_KEYBOARD:
                native_preconnect_ui_set_keyboard_available(ui, false);
                break;
            case NATIVE_INPUT_START_UNAVAILABLE:
                native_preconnect_ui_set_input_unavailable(ui);
                break;
            }
            /* HUB and setup use the system arrow and can leave it at an unrelated UI
             * coordinate. Restore the server cursor and home the compositor pointer to
             * the last RDP position before input starts flowing again. */
            native_cursor_reassert(&active->cursor);
            app->cursor_reassert_pending = true;
            SDL_WarpMouseInWindow(window, atomic_load(&app->virtual_mouse_x),
                                  atomic_load(&app->virtual_mouse_y));
            app->streaming_visible = true;
            app->hub_visible = false;
            native_system_volume_rebaseline(app);
            native_show_session_indicator(app, atomic_load(&app->active_index));
        }

        if (app->streaming_visible) {
            native_present_streaming_frame(app, renderer, &present_logged);
        }

        int activate_slot = -1;
        bool hub_close_requested = native_preconnect_ui_take_hub_close(ui);
        if (hub_close_requested) {
            /* BACK wins over every navigation action queued in the same LVGL input
             * batch, including Connect/Retry on an offline card. */
            native_preconnect_ui_cancel_pending_navigation(ui);
            if (!app->hub_visible) {
                /* On the initial pre-connect screen BACK has no desktop to return to;
                 * keep the HUB usable instead of leaving its close guard armed. */
                native_preconnect_ui_cancel_hub_close(ui);
            }
        }
        if (hub_close_requested && app->hub_visible) {
            int return_slot = app->hub_return_slot;
            bool return_slot_live = false;
            if (return_slot >= 0 && return_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
                NativeSessionSlot *return_session = &app->sessions[return_slot];
                return_slot_live = native_slot_is_live_stream_target(return_session);
            }
            if (!return_slot_live) {
                return_slot = -1;
                for (int candidate = 0; candidate < NATIVE_SETTINGS_MAX_SESSIONS; candidate++) {
                    NativeSessionSlot *candidate_slot = &app->sessions[candidate];
                    if (native_slot_is_live_stream_target(candidate_slot) &&
                        atomic_load(&candidate_slot->current_state) == (int)RDP_STATE_ACTIVE) {
                        return_slot = candidate;
                        break;
                    }
                }
                for (int candidate = 0;
                     return_slot < 0 && candidate < NATIVE_SETTINGS_MAX_SESSIONS; candidate++) {
                    NativeSessionSlot *candidate_slot = &app->sessions[candidate];
                    if (native_slot_is_live_stream_target(candidate_slot) &&
                        candidate_slot->snapshot_pending) {
                        return_slot = candidate;
                    }
                }
            }
            if (return_slot >= 0) {
                clog(cLogLevelInfo, "remote BACK closes HUB and returns to the %s profile",
                     native_session_slot_name(return_slot));
                native_request_session_switch(app, return_slot);
                if (app->hub_visible) {
                    /* The worker can leave ACTIVE between validation and the switch;
                     * release the guard when no stream accepted the return request. */
                    native_preconnect_ui_cancel_hub_close(ui);
                }
            } else {
                clog(cLogLevelWarning, "remote BACK cannot close HUB: no live session remains");
                native_preconnect_ui_cancel_hub_close(ui);
            }
        }
        if (!hub_close_requested && native_preconnect_ui_take_activate(ui, &activate_slot)) {
            native_request_session_switch(app, activate_slot);
        }

        int saved_slot = -1;
        uint16_t saved_audio_codec = settings->audio_codec;
        if (native_preconnect_ui_take_save(ui, &saved_slot, &saved_audio_codec) && saved_slot >= 0 &&
            saved_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
            NativeSessionConfig edited;
            bool saved = false;
            char save_status[128] = "";
            if (native_preconnect_ui_get_slot_values(ui, saved_slot, &edited)) {
                edited.duck_mask = settings->sessions[saved_slot].duck_mask;
                NativeSettings candidate = *settings;
                candidate.sessions[saved_slot] = edited;
                candidate.audio_codec = saved_audio_codec;
                NativeSessionSlot *runtime = &app->sessions[saved_slot];
                bool connection_changed =
                    native_session_connection_config_changed(&runtime->config, &edited);
                if (connection_changed && native_session_is_still_connecting(runtime)) {
                    (void)snprintf(save_status, sizeof(save_status),
                                   "The %s session is still connecting.",
                                   native_session_slot_name(saved_slot));
                } else if (native_config_save_persisted(&candidate)) {
                    bool endpoint_changed = native_session_endpoint_changed(&runtime->config, &edited);
                    *settings = candidate;
                    if (app->audio_codec != saved_audio_codec) {
                        clog(cLogLevelInfo, "audio codec preference now %s (new connections only)",
                             saved_audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto");
                        app->audio_codec = saved_audio_codec;
                    }
                    if (runtime->rdp && connection_changed) {
                        /* Never leave the live old endpoint behind a newly saved
                         * identity. Save stops it; Save and connect replaces it. */
                        native_stop_slot(app, saved_slot);
                        native_preconnect_ui_set_slot_state(ui, saved_slot, NATIVE_PRECONNECT_SESSION_OFFLINE,
                                                            NULL);
                        if (!native_any_slot_connected(app)) {
                            native_stop_media(app);
                        }
                    }
                    if (!runtime->rdp) {
                        if (endpoint_changed) {
                            runtime->refresh_ineffective = false;
                        }
                        pthread_mutex_lock(&app->redaction_lock);
                        runtime->config = edited;
                        pthread_mutex_unlock(&app->redaction_lock);
                    } else {
                        /* A display-name-only edit is safe on the existing endpoint. */
                        pthread_mutex_lock(&app->redaction_lock);
                        (void)snprintf(runtime->config.name, sizeof(runtime->config.name), "%s", edited.name);
                        pthread_mutex_unlock(&app->redaction_lock);
                    }
                    saved = true;
                } else {
                    clog(cLogLevelError, "failed to persist the %s profile",
                         native_session_slot_name(saved_slot));
                    (void)snprintf(save_status, sizeof(save_status),
                                   "Could not save settings on this TV. Check storage and retry.");
                }
            }
            native_preconnect_ui_finish_save(ui, saved_slot, saved,
                                             saved ? "Saved on this TV."
                                                   : (save_status[0] ? save_status
                                                                     : "Could not save this profile."));
        }

        int deleted_slot = -1;
        if (native_preconnect_ui_take_delete(ui, &deleted_slot) && deleted_slot >= 0 &&
            deleted_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
            NativeSessionConfig empty = {0};
            empty.port = 3389;
            empty.fps = 60;
            NativeSettings candidate = *settings;
            candidate.sessions[deleted_slot] = empty;
            NativeSessionSlot *runtime = &app->sessions[deleted_slot];
            bool deleted = false;
            char delete_status[128] = "";
            if (native_session_is_still_connecting(runtime)) {
                (void)snprintf(delete_status, sizeof(delete_status),
                               "The %s session is still connecting.",
                               native_session_slot_name(deleted_slot));
            } else if (native_config_save_persisted(&candidate)) {
                deleted = true;
                native_stop_slot(app, deleted_slot);
                *settings = candidate;
                runtime->refresh_ineffective = false;
                pthread_mutex_lock(&app->redaction_lock);
                runtime->config = empty;
                pthread_mutex_unlock(&app->redaction_lock);
                app->duck_mask[deleted_slot] = 0;
                app->mixer_mute_mask &= (uint8_t)~(uint8_t)(1u << deleted_slot);
                app->mixer_solo_mask &= (uint8_t)~(uint8_t)(1u << deleted_slot);
                native_publish_effective_solo_mask(app);
                app->mixer_gain_db[deleted_slot] = 0;
                native_audio_pipeline_set_source_muted(&app->audio_pipeline, deleted_slot, false);
                native_audio_pipeline_set_source_gain(&app->audio_pipeline, deleted_slot,
                                                      native_ui_mixer_gain_db_to_q15(0));
                native_duck_retarget(app);
                native_preconnect_ui_set_slot_state(ui, deleted_slot,
                                                    NATIVE_PRECONNECT_SESSION_NOT_SET_UP, NULL);
                if (!native_any_slot_connected(app)) {
                    native_stop_media(app);
                }
            } else {
                clog(cLogLevelError, "failed to delete the %s profile",
                     native_session_slot_name(deleted_slot));
                (void)snprintf(delete_status, sizeof(delete_status),
                               "Could not delete this profile. Check storage and retry.");
            }
            native_preconnect_ui_finish_delete(ui, deleted_slot, deleted,
                                               deleted ? "Profile deleted."
                                                       : (delete_status[0] ? delete_status
                                                                           : "Could not delete this profile."));
        }

        int requested_slot = NATIVE_SESSION_SLOT_GREEN;
        char requested_host[NATIVE_CONFIG_STRING_MAX];
        char requested_username[NATIVE_CONFIG_STRING_MAX];
        char requested_password[NATIVE_CONFIG_STRING_MAX];
        char requested_domain[NATIVE_CONFIG_STRING_MAX];
        uint16_t requested_port = 0;
        uint16_t requested_fps = 0;
        uint16_t requested_audio_codec = NATIVE_AUDIO_CODEC_AUTO;
        bool requested_requires_save = false;
        /* ALWAYS consume the one-shot request: gating the take on slot state would leave
         * a Connect clicked on a still-connecting slot's configurator latched in the UI,
         * to be replayed as a surprise reconnect whenever that session later fails. */
        if (native_preconnect_ui_take_connect(ui, &requested_slot, requested_host, sizeof(requested_host),
                                              &requested_port, requested_username, sizeof(requested_username),
                                              requested_password, sizeof(requested_password), requested_domain,
                                              sizeof(requested_domain), &requested_fps, &requested_audio_codec,
                                              &requested_requires_save)) {
            if (requested_slot < 0 || requested_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
                requested_slot = NATIVE_SESSION_SLOT_GREEN;
            }
            NativeSessionSlot *requested_live = &app->sessions[requested_slot];
            NativeSettings candidate = *settings;
            NativeSessionConfig *session = &candidate.sessions[requested_slot];
            char validation_message[128] = "";
            bool ready_to_connect = true;
            if (native_session_is_still_connecting(requested_live)) {
                /* Mid-handshake: joining the worker here could block the SDL thread for
                 * the whole connect timeout, so reject instead. */
                (void)snprintf(validation_message, sizeof(validation_message),
                               "The %s session is still connecting.", native_session_slot_name(requested_slot));
                ready_to_connect = false;
            } else if (!copy_config_string(session->host, sizeof(session->host), requested_host, "host")) {
                (void)snprintf(validation_message, sizeof(validation_message), "Host value is too long.");
                ready_to_connect = false;
            } else if (!copy_config_string(session->username, sizeof(session->username), requested_username,
                                           "username")) {
                (void)snprintf(validation_message, sizeof(validation_message), "Username value is too long.");
                ready_to_connect = false;
            } else if (!copy_config_string(session->password, sizeof(session->password), requested_password,
                                           "password")) {
                (void)snprintf(validation_message, sizeof(validation_message), "Password value is too long.");
                ready_to_connect = false;
            } else if (!copy_config_string(session->domain, sizeof(session->domain), requested_domain, "domain")) {
                (void)snprintf(validation_message, sizeof(validation_message), "Domain value is too long.");
                ready_to_connect = false;
            } else {
                NativeSessionConfig edited;
                if (native_preconnect_ui_get_slot_values(ui, requested_slot, &edited)) {
                    (void)snprintf(session->name, sizeof(session->name), "%s", edited.name);
                }
                session->port = requested_port;
                session->fps = requested_fps;
                candidate.audio_codec = requested_audio_codec;
                native_config_apply_initial_desktop_hint(&candidate);
                if (!native_session_config_validate_connect(session, requested_slot, validation_message,
                                                            sizeof(validation_message))) {
                    ready_to_connect = false;
                }
            }

            if (ready_to_connect && requested_requires_save && !native_config_save_persisted(&candidate)) {
                clog(cLogLevelError, "failed to persist the %s profile before connect",
                     native_session_slot_name(requested_slot));
                (void)snprintf(validation_message, sizeof(validation_message),
                               "Could not save settings on this TV. Check storage and retry.");
                ready_to_connect = false;
            }
            native_preconnect_ui_finish_connect_save(ui, requested_slot, ready_to_connect,
                                                      ready_to_connect && requested_requires_save,
                                                      ready_to_connect ? NULL : validation_message);
            if (ready_to_connect) {
                /* A HUB-originated connect exits only after the asynchronous worker is
                 * ACTIVE. Until then the old stream remains the BACK destination. */
                bool connect_from_hub = app->hub_visible;
                app->hub_connect_target = connect_from_hub ? requested_slot : -1;
                if (requested_requires_save) {
                    *settings = candidate;
                }
                session = &settings->sessions[requested_slot];
                if (app->audio_codec != requested_audio_codec) {
                    clog(cLogLevelInfo, "audio codec preference now %s (new connections only)",
                         requested_audio_codec == NATIVE_AUDIO_CODEC_PCM ? "pcm" : "auto");
                    app->audio_codec = requested_audio_codec;
                }
                native_config_log_effective(settings);
                native_preconnect_ui_set_connecting(ui, requested_slot, true, "Connecting...");
                int old_index = atomic_load(&app->active_index);
                /* Join before replacing config: workers may still consult their old
                 * redaction strings while winding down. This is an explicit user
                 * connection, not an internal recovery reconnect, so it starts a new
                 * logical-session clock once the replacement reaches ACTIVE. */
                app->session_started_ms[requested_slot] = 0;
                app->session_runtime_active[requested_slot] = false;
                native_stop_slot(app, requested_slot);
                if (native_session_endpoint_changed(&app->sessions[requested_slot].config, session)) {
                    app->sessions[requested_slot].refresh_ineffective = false;
                }
                pthread_mutex_lock(&app->redaction_lock);
                app->sessions[requested_slot].config = *session;
                pthread_mutex_unlock(&app->redaction_lock);

                /* Ask the outgoing server to stop graphics before ownership moves, so
                 * no full-rate AUs/bitmap deltas can land in the demotion window. Do
                 * not mark it suppressed yet: after the flip native_background_slot()
                 * still needs to choose plain suppress vs hidden snapshot reconnect. */
                if (old_index != requested_slot && app->sessions[old_index].rdp &&
                    !app->sessions[old_index].suppressed) {
                    rdp_set_suppress_output(app->sessions[old_index].rdp, false);
                }
                app->pending_switch_target = -1;
                app->switch_deadline_ticks = 0;
                pthread_mutex_lock(&app->video_lock);
                if (connect_from_hub) {
                    native_capture_hub_return_rgba_locked(app);
                }
                if (app->hub_return_rgba) {
                    /* native_slot_connect increments this epoch before the worker starts.
                     * Arm the readiness gate now so an immediate callback cannot make us
                     * drop the frozen background before ACTIVE. */
                    native_arm_hub_return_replacement_locked(
                        app, requested_slot,
                        atomic_load(&app->sessions[requested_slot].connect_epoch) + 1u,
                        atomic_load(&app->sessions[requested_slot].video_ok_frames));
                }
                /* Any non-frozen canvas belongs to the outgoing/failed owner. The new
                 * RemoteFX stream must receive a zeroed surface for its dirty rectangles. */
                native_close_rgba_locked(app, false);
                atomic_store(&app->active_index, requested_slot);
                atomic_store(&app->video_refresh_needed, false);
                app->video_plane_punched = false;
                pthread_mutex_unlock(&app->video_lock);
                native_preconnect_ui_select_slot(ui, requested_slot);
                native_duck_retarget(app);
                if (old_index != requested_slot) {
                    /* Demotion must precede this helper: snapshot backgrounding may
                     * reconnect old_index, which must not reset the shared decoder. */
                    native_background_slot(app, old_index);
                }
                app->ui_last_state = -1;
                if (!native_slot_connect(app, requested_slot, false)) {
                    const char *start_error = "Connection failed: start failed";
                    if (app->hub_connect_target == requested_slot) {
                        app->hub_connect_target = -1;
                    }
                    native_preconnect_ui_set_connecting(ui, requested_slot, false, start_error);
                    native_preconnect_ui_set_slot_state(ui, requested_slot, NATIVE_PRECONNECT_SESSION_ERROR,
                                                        start_error);
                    native_preconnect_ui_open_setup(ui, requested_slot);
                    native_preconnect_ui_set_status(ui, start_error, true);
                    if (!native_any_slot_connected(app)) {
                        native_stop_media(app);
                    }
                } else {
                    native_update_pointer_window_size(app);
                }
            }
        }

        pthread_mutex_lock(&app->video_lock);
        bool hardware_video_plane = app->video != NULL;
        pthread_mutex_unlock(&app->video_lock);
        native_preconnect_ui_set_hardware_video_plane(ui, hardware_video_plane);
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
    if (!native_slot_connect(app, NATIVE_SESSION_SLOT_GREEN, false)) {
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
     * The flag still gates active-session input and presentation. */
    app->streaming_visible = true;
    native_system_volume_rebaseline(app);

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
        native_system_volume_tick(app);

        NativeSessionSlot *arm_slot = native_active_slot(app);
        /* Same overlay gate as the preconnect loop: overlay navigation must not leak
         * input to the session behind it. */
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
        native_destroy_rgba_renderer_textures(app);
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    return atomic_load(&app->exit_code);
}
#else
static int native_run_app_loop(App *app, NativeSettings *settings) {
    (void)settings;
    clog(cLogLevelInfo,
         "SDL event loop is not compiled in; deterministic smoke loop exits after start "
         "(enable HELLOLG_WITH_SDL for webOS lifecycle/input)");
    return atomic_load(&app->exit_code);
}
#endif

int main(int argc, char **argv) {
    native_prepare_webos_logging();
    (void)clog_configure_env();
    clog(cLogLevelNotice, "--- launch ---");
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
    atomic_init(&app.active_index, NATIVE_SESSION_SLOT_GREEN);
    app.rgba_owner_slot = -1;
    app.hub_return_rgba_owner_slot = -1;
    app.hub_return_replacement_slot = -1;
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
        atomic_init(&slot->audio_codec, 0u);
        atomic_init(&slot->audio_sample_rate, 0u);
        atomic_init(&slot->audio_channels, 0u);
        atomic_init(&slot->connect_epoch, 0u);
        atomic_init(&slot->keyframe_wait_drops, 0u);
        atomic_init(&slot->snapshot_idr_ready, false);
        atomic_init(&slot->snapshot_last_au_ms, 0u);
        atomic_init(&slot->video_via_bitmap, false);
        app.mixer_gain_db[i] = 0; /* unity */
        app.duck_mask[i] = (uint8_t)native_settings.sessions[i].duck_mask;
    }
    atomic_init(&app.video_refresh_needed, false);
    app.audio_codec = native_settings.audio_codec;
    if (!native_audio_pipeline_init(&app.audio_pipeline) ||
        !native_audio_pipeline_pump_start(&app.audio_pipeline, native_audio_pipeline_feed_cb, &app)) {
        clog(cLogLevelWarning, "miniaudio pipeline unavailable; continuing with silent video");
        native_audio_pipeline_destroy(&app.audio_pipeline);
    } else {
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            native_audio_pipeline_set_source_gain(&app.audio_pipeline, i,
                                                  native_ui_mixer_gain_db_to_q15(app.mixer_gain_db[i]));
        }
        clog(cLogLevelInfo, "miniaudio engine running at 48000Hz stereo, 480-frame blocks");
    }
    app.settings = &native_settings;
    app.mixer_mute_mask = 0;
    app.mixer_solo_mask = 0;
    native_duck_retarget(&app);
    const char *snapshot_force = getenv("HELLOLG_SNAPSHOT_FORCE");
    app.snapshot_force = snapshot_force && snapshot_force[0] != '\0' && strcmp(snapshot_force, "0") != 0;
    if (app.snapshot_force) {
        clog(cLogLevelInfo, "HELLOLG_SNAPSHOT_FORCE: IDR-snapshot backgrounding for every slot");
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    app.indicator_slot = -1;
    app.system_volume_seen = -1;
    app.system_volume_baseline_seq = 0;
    app.pending_switch_target = -1;
    app.hub_return_slot = -1;
    app.hub_connect_target = -1;
    app.wheel_step = native_settings.wheel_step;
    app.wheel_scroll_divisor = native_settings.wheel_scroll_divisor;
    atomic_init(&app.render_width, 0);
    atomic_init(&app.render_height, 0);
#endif
    atomic_init(&app.running, true);
    atomic_init(&app.exit_code, 0);
    native_input_init(&app.input, NULL, native_settings.width, native_settings.height);
    /* System-volume bridge for the MASTER fader; harmless where luna-send-pub does not
     * exist (every call fails, the fader just stays dimmed). */
    if (!native_luna_volume_start(&app.luna_volume)) {
        clog(cLogLevelWarning, "volume worker failed to start; the MASTER fader stays unavailable");
    }
    native_luna_volume_refresh(&app.luna_volume);

#if !defined(HELLOLG_WITH_SDL) || !HELLOLG_WITH_SDL
    app.sessions[NATIVE_SESSION_SLOT_GREEN].config = native_settings.sessions[NATIVE_SESSION_SLOT_GREEN];
    if (!native_slot_connect(&app, NATIVE_SESSION_SLOT_GREEN, false)) {
        int exit_code = atomic_load(&app.exit_code);
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            native_cursor_destroy(&app.sessions[i].cursor);
        }
        native_audio_pipeline_destroy(&app.audio_pipeline);
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
    native_luna_volume_stop(&app.luna_volume);
    /* The pump feeds through video_lock; stop it before destroying that lock. */
    native_audio_pipeline_destroy(&app.audio_pipeline);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        native_cursor_destroy(&app.sessions[i].cursor);
    }
    pthread_mutex_destroy(&app.redaction_lock);
    pthread_mutex_destroy(&app.video_lock);
    native_shutdown_sdl_runtime();

    int exit_code = atomic_load(&app.exit_code);
    if (exit_code == 0 && app.decoder_errors > 0) {
        exit_code = rdp_state_exit_code(RDP_STATE_DECODER_ERROR);
    }
    return exit_code;
}
