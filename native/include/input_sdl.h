#ifndef GNOMECAST_INPUT_SDL_H
#define GNOMECAST_INPUT_SDL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "rdp_ffi.h"

typedef enum NativeInputButton {
    NATIVE_INPUT_BUTTON_LEFT = 1,
    NATIVE_INPUT_BUTTON_RIGHT = 2,
    NATIVE_INPUT_BUTTON_MIDDLE = 3
} NativeInputButton;

typedef struct NativeInput {
    RdpSession *session;
    /* Written from the RDP worker thread (on_desktop_size) and/or the SDL thread
     * (window resize, local surface change) and read from the SDL thread on every
     * pointer event; must stay atomic to avoid torn reads of the mapping ratio. */
    atomic_uint desktop_width;
    atomic_uint desktop_height;
    atomic_uint window_width;
    atomic_uint window_height;
    uint16_t last_x;
    uint16_t last_y;
    atomic_bool active;
} NativeInput;

void native_input_init(NativeInput *input, RdpSession *session, uint16_t desktop_width, uint16_t desktop_height);
void native_input_set_session(NativeInput *input, RdpSession *session);
void native_input_set_active(NativeInput *input, bool active);
bool native_input_is_active(const NativeInput *input);
void native_input_set_desktop_size(NativeInput *input, uint16_t desktop_width, uint16_t desktop_height);
void native_input_set_window_size(NativeInput *input, uint16_t window_width, uint16_t window_height);
void native_input_map_point(const NativeInput *input, int window_x, int window_y, uint16_t *rdp_x, uint16_t *rdp_y);
bool native_input_pointer_move(NativeInput *input, int window_x, int window_y);
bool native_input_pointer_button(NativeInput *input, int window_x, int window_y, NativeInputButton button, bool down);
bool native_input_pointer_wheel(NativeInput *input, int window_x, int window_y, int16_t delta);
bool native_input_key(NativeInput *input, uint8_t scancode, bool down, bool extended);
bool native_input_unicode(NativeInput *input, uint16_t codepoint, bool down);
bool native_input_sync_locks(NativeInput *input, bool scroll_lock, bool num_lock, bool caps_lock);
bool native_input_sdl_scancode_to_rdp(uint32_t sdl_scancode, uint8_t *rdp_scancode, bool *extended);
bool native_input_text_utf8(NativeInput *input, const char *utf8);

/* Pointer-jump filter: the webOS compositor warps the system pointer to the screen
 * center around IME show/hide transitions (there is no setting to disable it). The warp
 * has a precise signature — a single SDL_MOUSEMOTION landing at the window center with a
 * large delta — so it is detected by coordinates, not by a time window that would also
 * penalize legitimate movement while typing. Real devices report small per-event deltas,
 * and a genuine glide to the center ends with a small final delta, so neither trips it. */
#define NATIVE_INPUT_JUMP_ALWAYS_PX 500
#define NATIVE_INPUT_CENTER_JUMP_MIN_PX 40
#define NATIVE_INPUT_CENTER_TOLERANCE_PX 2
/* The warp can coalesce with concurrent real movement (typing while moving the mouse),
 * landing near — not exactly at — the center. Second tier: a landing within this radius
 * combined with a delta no real per-event motion reaches is still the recenter. */
#define NATIVE_INPUT_CENTER_RADIUS_PX 100
#define NATIVE_INPUT_CENTER_RADIUS_JUMP_PX 200

/* True for a delta no physical device produces in one event (coarse net, any mode). */
bool native_input_motion_is_jump(int dx, int dy);
/* True when a single motion of (dx, dy) lands within tolerance of the window center —
 * the webOS IME recenter signature (absolute mode). */
bool native_input_motion_is_center_jump(int x, int y, int dx, int dy, uint16_t window_w,
                                        uint16_t window_h);

#endif
