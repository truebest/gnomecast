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
bool native_input_sdl_scancode_to_rdp(uint32_t sdl_scancode, uint8_t *rdp_scancode, bool *extended);
bool native_input_text_utf8(NativeInput *input, const char *utf8);

#endif
