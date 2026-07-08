#ifndef GNOMECAST_UI_MIXER_H
#define GNOMECAST_UI_MIXER_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_mixer.h"
#include "settings_json.h"

/* Volume-mixer overlay: one channel per session slot — a dBFS fader (bottom stop = full
 * mute, an unmarked +6 dB headroom above the 0 line) over an L/R pair of live post-fader
 * volume meters. This module owns everything visual: the LVGL screen shown over the
 * punched video plane (preconnect-UI builds; the screen shares the preconnect display)
 * and the raw-SDL fallback panel, plus the dB fader model the input handling in main.c
 * shares. Opening/closing, key routing and the per-slot gain storage stay in main.c. */

/* Fader model: 3 dB steps from -60 (= mute) up to +6; the overlay auto-hides after this
 * long without a key press. Meters: instant attack, steady dB/s release, with a floor
 * comfortably below the visible scale. */
#define NATIVE_MIXER_FADER_MIN_DB (-60)
#define NATIVE_MIXER_FADER_MAX_DB 6
#define NATIVE_MIXER_OVERLAY_GAIN_STEP_DB 3
#define NATIVE_MIXER_OVERLAY_IDLE_HIDE_MS 6000u
#define NATIVE_MIXER_METER_DECAY_DB_S 30.0f
#define NATIVE_MIXER_METER_FLOOR_DB (-90.0f)

/* 3 dB-step fader position NATIVE_MIXER_FADER_MIN_DB..MAX_DB -> Q15 mixer gain
 * (round(32768 * 10^(dB/20)); a lookup table keeps libm out of the shared code path).
 * The bottom stop maps to 0: the fader floor is a full mute, not an audible -60 dB. */
int32_t native_ui_mixer_gain_db_to_q15(int gain_db);

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
#include <SDL.h>

/* Raw-SDL fallback panel for builds without the LVGL preconnect UI: latch-drawn (the
 * caller repaints on interaction only) rails + capsule knob per slot, tick marks, no
 * meters. Clears the window to the transparent punch and presents one frame. Bit i of
 * `connected_mask` marks slot i's session ACTIVE (dimmed otherwise). */
void native_ui_mixer_draw_fallback(SDL_Renderer *renderer, const int8_t *gain_db, int selected,
                                   unsigned connected_mask);

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI

typedef struct NativeUiMixer NativeUiMixer;

/* Created (and destroyed) by the preconnect UI, whose LVGL display the overlay screen
 * shares — create() picks it up via lv_disp_get_default(), so it must run after that
 * display is registered. The host must keep the screen texture current across resizes
 * (set_texture); main.c drives show/hide/render via native_preconnect_ui_mixer(). All
 * functions are NULL-tolerant so a failed create degrades to the fallback panel. */
NativeUiMixer *native_ui_mixer_create(SDL_Renderer *renderer);
void native_ui_mixer_destroy(NativeUiMixer *mixer);
void native_ui_mixer_set_texture(NativeUiMixer *mixer, SDL_Texture *texture);

/* Loads the overlay screen, remembering the active one to restore on hide, and drops the
 * display background so only the panel paints — the video plane fills the rest. */
void native_ui_mixer_show(NativeUiMixer *mixer);
void native_ui_mixer_hide(NativeUiMixer *mixer);
bool native_ui_mixer_active(const NativeUiMixer *mixer);

/* Per-frame while the overlay is up: applies the meter ballistics to the caller-supplied
 * post-fader peaks (one [L,R] pair per slot — the caller snapshots them from the audio
 * mixer under its lifecycle lock, so this render path never touches mixer internals that
 * a format re-pin may be destroying), touches only widgets whose values changed (so LVGL
 * repaints just those areas) and paints immediately via lv_refr_now — no LVGL timer pump
 * runs during streaming. `gain_db` holds NATIVE_SETTINGS_MAX_SESSIONS fader positions;
 * `now_ticks` = SDL_GetTicks(). */
void native_ui_mixer_render(NativeUiMixer *mixer, const int32_t (*peaks)[2], const int8_t *gain_db, int selected,
                            unsigned connected_mask, uint32_t now_ticks);

/* Pointer support (LVGL panel layout; pure arithmetic, window coordinates). hit_test:
 * true when (x,y) lands inside the panel, with *slot the channel under x (or -1 over
 * padding/gaps) and *on_fader whether y is on the fader track (a click there jumps the
 * knob; elsewhere it only selects). fader_db_at: the fader value for y — clamped to the
 * track and snapped onto the 3 dB steps — for click-jump and drag. */
bool native_ui_mixer_hit_test(int win_w, int win_h, int x, int y, int *slot, bool *on_fader);
int native_ui_mixer_fader_db_at(int win_h, int y);

#endif /* HELLOLG_WITH_PRECONNECT_UI */
#endif /* HELLOLG_WITH_SDL */

#endif
