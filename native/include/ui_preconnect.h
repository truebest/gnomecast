#ifndef GNOMECAST_UI_PRECONNECT_H
#define GNOMECAST_UI_PRECONNECT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <SDL.h>

#include "settings_json.h"
#include "ui_mixer.h"

typedef struct NativePreconnectUi NativePreconnectUi;

/* The form edits one session slot at a time (green/yellow, matching the remote's color
 * buttons); the nav panel and native_preconnect_ui_select_slot switch between them. The
 * audio-buffer slider is app-global. `sessions` must hold NATIVE_SETTINGS_MAX_SESSIONS
 * entries; they are copied. */
NativePreconnectUi *native_preconnect_ui_create(SDL_Window *window, SDL_Renderer *renderer,
                                                const NativeSessionConfig *sessions, uint16_t audio_prebuffer_ms,
                                                uint16_t audio_codec);
void native_preconnect_ui_destroy(NativePreconnectUi *ui);
void native_preconnect_ui_resize(NativePreconnectUi *ui, int width, int height);
void native_preconnect_ui_tick(NativePreconnectUi *ui);
void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible);
void native_preconnect_ui_set_connecting(NativePreconnectUi *ui, bool connecting, const char *status);
void native_preconnect_ui_set_status(NativePreconnectUi *ui, const char *status, bool error);
/* Shows `slot`'s stored values in the form (the current form is saved to its own slot
 * first). Green/yellow remote buttons and the nav-panel buttons both land here. */
void native_preconnect_ui_select_slot(NativePreconnectUi *ui, int slot);
bool native_preconnect_ui_read_current(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_prebuffer_ms,
                                       uint16_t *audio_codec);
/* Copies slot `slot`'s stored form values (parsed host/port, credentials, fps) into
 * *out. Returns false — leaving *out untouched — for an out-of-range slot or when the
 * slot's endpoint is mid-edit (unparseable), so callers keep their previous values. */
bool native_preconnect_ui_get_slot_values(NativePreconnectUi *ui, int slot, NativeSessionConfig *out);
/* One-shot: returns true once per Connect press; *slot tells which session the values
 * belong to. */
bool native_preconnect_ui_take_connect(NativePreconnectUi *ui, int *slot, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_prebuffer_ms,
                                       uint16_t *audio_codec);

/* The volume-mixer overlay (ui_mixer.h) shares this UI's LVGL display and is owned by
 * it; main.c drives the returned handle directly. NULL when its creation failed —
 * native_ui_mixer_* calls tolerate that and main.c falls back to its raw-SDL panel. */
NativeUiMixer *native_preconnect_ui_mixer(NativePreconnectUi *ui);

#endif
