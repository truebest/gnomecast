#ifndef GNOMECAST_UI_PRECONNECT_H
#define GNOMECAST_UI_PRECONNECT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <SDL.h>

#include "settings_json.h"
#include "ui_mixer.h"

typedef struct NativePreconnectUi NativePreconnectUi;
typedef bool (*NativePreconnectUiBackgroundDrawFn)(void *ctx, SDL_Renderer *renderer);

typedef enum NativePreconnectSessionState {
    NATIVE_PRECONNECT_SESSION_NOT_SET_UP = 0,
    NATIVE_PRECONNECT_SESSION_OFFLINE,
    NATIVE_PRECONNECT_SESSION_CONNECTING,
    NATIVE_PRECONNECT_SESSION_CONNECTED,
    NATIVE_PRECONNECT_SESSION_ERROR,
} NativePreconnectSessionState;

/* The 1920x1080 hub presents four fixed red/green/yellow/blue profiles and opens one
 * profile at a time in its setup drawer. The audio-codec preference is app-global.
 * `sessions` must hold NATIVE_SETTINGS_MAX_SESSIONS entries; they are copied. */
NativePreconnectUi *native_preconnect_ui_create(SDL_Window *window, SDL_Renderer *renderer,
                                                const NativeSessionConfig *sessions, uint16_t audio_codec);
void native_preconnect_ui_destroy(NativePreconnectUi *ui);
void native_preconnect_ui_resize(NativePreconnectUi *ui, int width, int height);
void native_preconnect_ui_tick(NativePreconnectUi *ui);
void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible);
/* HUB background plumbing: the callback may draw a cached software-decoded desktop
 * into the window backbuffer (without presenting). Otherwise `hardware_video_plane`
 * controls whether the backbuffer is cleared transparent for NDL or opaque for setup. */
void native_preconnect_ui_set_background_drawer(NativePreconnectUi *ui,
                                                NativePreconnectUiBackgroundDrawFn draw, void *ctx);
void native_preconnect_ui_set_hardware_video_plane(NativePreconnectUi *ui, bool available);
void native_preconnect_ui_set_connecting(NativePreconnectUi *ui, int slot, bool connecting, const char *status);
/* Drawer-only status text. Session cards are updated explicitly with set_slot_state. */
void native_preconnect_ui_set_status(NativePreconnectUi *ui, const char *status, bool error);
/* Updates one hub card from main.c's session state. `detail` is copied and may be NULL. */
void native_preconnect_ui_set_slot_state(NativePreconnectUi *ui, int slot,
                                         NativePreconnectSessionState state, const char *detail);
/* Updates one card's live stream metadata. `session_minutes` counts the logical user
 * session across hidden recovery reconnects; an open audio stream carries the negotiated
 * RdpAudioCodec, sample rate, and channel count. Until that tuple arrives, HUB keeps the
 * audio route in its waiting state instead of hiding it. Peaks are post-fader Q15 L/R
 * snapshots for the compact card VU meter. */
void native_preconnect_ui_set_slot_runtime(NativePreconnectUi *ui, int slot, uint16_t desktop_width,
                                           uint16_t desktop_height, uint32_t session_minutes,
                                           bool audio_stream_open, uint32_t audio_codec,
                                           uint32_t audio_sample_rate, uint16_t audio_channels,
                                           int32_t audio_peak_left, int32_t audio_peak_right);
void native_preconnect_ui_set_keyboard_available(NativePreconnectUi *ui, bool available);
void native_preconnect_ui_set_mouse_available(NativePreconnectUi *ui, bool available);
void native_preconnect_ui_set_input_unavailable(NativePreconnectUi *ui);
int native_preconnect_ui_selected_slot(const NativePreconnectUi *ui);
/* Remote session shortcuts are suspended while onboarding, connection, or persistence owns the screen. */
bool native_preconnect_ui_session_keys_enabled(const NativePreconnectUi *ui);
/* Selects `slot` on the hub without opening its editor. An open editor's uncommitted
 * form is discarded; Save and Save-and-connect retain their explicit snapshots. */
void native_preconnect_ui_select_slot(NativePreconnectUi *ui, int slot);
/* Returns from a live desktop to the hub, selecting its active profile and closing
 * any setup/onboarding layer that would otherwise obscure the four cards. */
void native_preconnect_ui_show_hub(NativePreconnectUi *ui, int active_slot);
/* Selects the next/previous configured profile on the hub. */
bool native_preconnect_ui_cycle_slot(NativePreconnectUi *ui, int direction);
/* Selects `slot` and opens its right-side setup drawer. */
void native_preconnect_ui_open_setup(NativePreconnectUi *ui, int slot);
/* Queues a configured profile directly from a remote colour key; incomplete profiles
 * stay in the setup drawer. */
bool native_preconnect_ui_request_connect(NativePreconnectUi *ui, int slot);
/* A newer remote colour-key action supersedes an unconsumed Resume/Connect action. */
void native_preconnect_ui_cancel_pending_navigation(NativePreconnectUi *ui);
bool native_preconnect_ui_read_current(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_codec);
/* Copies slot `slot`'s stored name, host/port, credentials, fps, and duck mask into
 * *out. Returns false (leaving *out untouched) for an out-of-range slot or invalid port. */
bool native_preconnect_ui_get_slot_values(NativePreconnectUi *ui, int slot, NativeSessionConfig *out);
/* One-shot: returns true once per Connect press; *slot tells which session the values
 * belong to. */
bool native_preconnect_ui_take_connect(NativePreconnectUi *ui, int *slot, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_codec,
                                       bool *requires_save);
/* One-shot hub actions. Activate asks main.c to resume an already-connected slot; Save
 * persists edited values and lets main.c stop a live worker when connection fields changed. */
bool native_preconnect_ui_take_activate(NativePreconnectUi *ui, int *slot);
/* One-shot BACK request from the unobscured HUB. Nested setup/onboarding layers consume
 * their own first BACK and do not arm this request. */
bool native_preconnect_ui_take_hub_close(NativePreconnectUi *ui);
/* Re-enables HUB navigation when a close request cannot return to any live session. */
void native_preconnect_ui_cancel_hub_close(NativePreconnectUi *ui);
bool native_preconnect_ui_take_save(NativePreconnectUi *ui, int *slot, uint16_t *audio_codec);
bool native_preconnect_ui_take_delete(NativePreconnectUi *ui, int *slot);
/* Persistence acknowledgements keep the drawer honest when storage is unavailable. */
void native_preconnect_ui_finish_save(NativePreconnectUi *ui, int slot, bool success, const char *status);
void native_preconnect_ui_finish_connect_save(NativePreconnectUi *ui, int slot, bool success, bool persisted,
                                               const char *status);
void native_preconnect_ui_finish_delete(NativePreconnectUi *ui, int slot, bool success, const char *status);

/* The volume-mixer overlay (ui_mixer.h) shares this UI's LVGL display and is owned by
 * it; main.c drives the returned handle directly. NULL when its creation failed;
 * native_ui_mixer_* calls tolerate that and main.c leaves the overlay unavailable. */
NativeUiMixer *native_preconnect_ui_mixer(NativePreconnectUi *ui);

#endif
