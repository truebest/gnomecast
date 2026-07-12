#include "input_sdl.h"

#include <stddef.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_input, cLogLevelInfo, cLogFlags_Default, "input.sdl", NULL);

static uint16_t clamp_dimension(uint16_t value) {
    return value == 0 ? 1 : value;
}

static bool native_input_ready(const NativeInput *input) {
    return input && atomic_load(&input->active) && input->session;
}

void native_input_init(NativeInput *input, RdpSession *session, uint16_t desktop_width, uint16_t desktop_height) {
    if (!input) {
        clog_limited(cLogLevelWarning, 2, 5000, "cannot initialize NULL input state");
        return;
    }
    memset(input, 0, sizeof(*input));
    input->session = session;
    atomic_init(&input->active, false);
    atomic_init(&input->desktop_width, 1u);
    atomic_init(&input->desktop_height, 1u);
    atomic_init(&input->window_width, 1u);
    atomic_init(&input->window_height, 1u);
    native_input_set_desktop_size(input, desktop_width, desktop_height);
    native_input_set_window_size(input, desktop_width, desktop_height);
    clog(cLogLevelDebug, "initialized SDL input (session=%s desktop=%ux%u)", session ? "set" : "unset",
         (unsigned)clamp_dimension(desktop_width), (unsigned)clamp_dimension(desktop_height));
}

void native_input_set_session(NativeInput *input, RdpSession *session) {
    if (!input) {
        return;
    }
    RdpSession *previous = input->session;
    input->session = session;
    if (previous != session) {
        clog(cLogLevelDebug, "SDL input session %s", session ? "attached" : "detached");
    }
}

void native_input_set_active(NativeInput *input, bool active) {
    if (!input) {
        return;
    }
    bool was_active = atomic_exchange(&input->active, active);
    if (was_active != active) {
        clog(cLogLevelDebug, "SDL input %s", active ? "activated" : "deactivated");
    }
}

bool native_input_is_active(const NativeInput *input) {
    return input && atomic_load(&input->active);
}

void native_input_set_desktop_size(NativeInput *input, uint16_t desktop_width, uint16_t desktop_height) {
    if (!input) {
        return;
    }
    uint16_t width = clamp_dimension(desktop_width);
    uint16_t height = clamp_dimension(desktop_height);
    unsigned previous_width = atomic_exchange(&input->desktop_width, width);
    unsigned previous_height = atomic_exchange(&input->desktop_height, height);
    if (previous_width != width || previous_height != height) {
        clog(cLogLevelDebug, "desktop input extent=%ux%u", (unsigned)width, (unsigned)height);
    }
}

void native_input_set_window_size(NativeInput *input, uint16_t window_width, uint16_t window_height) {
    if (!input) {
        return;
    }
    uint16_t width = clamp_dimension(window_width);
    uint16_t height = clamp_dimension(window_height);
    unsigned previous_width = atomic_exchange(&input->window_width, width);
    unsigned previous_height = atomic_exchange(&input->window_height, height);
    if (previous_width != width || previous_height != height) {
        clog(cLogLevelDebug, "window input extent=%ux%u", (unsigned)width, (unsigned)height);
    }
}

void native_input_map_point(const NativeInput *input, int window_x, int window_y, uint16_t *rdp_x, uint16_t *rdp_y) {
    uint16_t ww = clamp_dimension(input ? (uint16_t)atomic_load(&input->window_width) : 1);
    uint16_t wh = clamp_dimension(input ? (uint16_t)atomic_load(&input->window_height) : 1);
    uint16_t dw = clamp_dimension(input ? (uint16_t)atomic_load(&input->desktop_width) : 1);
    uint16_t dh = clamp_dimension(input ? (uint16_t)atomic_load(&input->desktop_height) : 1);

    if (window_x < 0) {
        window_x = 0;
    }
    if (window_y < 0) {
        window_y = 0;
    }
    if (window_x >= ww) {
        window_x = (int)ww - 1;
    }
    if (window_y >= wh) {
        window_y = (int)wh - 1;
    }

    uint32_t x = ((uint32_t)window_x * (uint32_t)dw) / (uint32_t)ww;
    uint32_t y = ((uint32_t)window_y * (uint32_t)dh) / (uint32_t)wh;
    if (x >= dw) {
        x = (uint32_t)dw - 1;
    }
    if (y >= dh) {
        y = (uint32_t)dh - 1;
    }
    if (rdp_x) {
        *rdp_x = (uint16_t)x;
    }
    if (rdp_y) {
        *rdp_y = (uint16_t)y;
    }
}

bool native_input_pointer_move(NativeInput *input, int window_x, int window_y) {
    if (!native_input_ready(input)) {
        return false;
    }
    native_input_map_point(input, window_x, window_y, &input->last_x, &input->last_y);
    rdp_send_pointer_move(input->session, input->last_x, input->last_y);
    return true;
}

bool native_input_pointer_button(NativeInput *input, int window_x, int window_y, NativeInputButton button, bool down) {
    if (!native_input_ready(input)) {
        return false;
    }
    native_input_map_point(input, window_x, window_y, &input->last_x, &input->last_y);
    rdp_send_pointer_button(input->session, input->last_x, input->last_y, (uint8_t)button, down);
    return true;
}

bool native_input_pointer_wheel(NativeInput *input, int window_x, int window_y, int16_t delta) {
    if (!native_input_ready(input)) {
        return false;
    }
    native_input_map_point(input, window_x, window_y, &input->last_x, &input->last_y);
    rdp_send_pointer_wheel(input->session, input->last_x, input->last_y, delta);
    return true;
}

bool native_input_key(NativeInput *input, uint8_t scancode, bool down, bool extended) {
    if (!native_input_ready(input)) {
        return false;
    }
    rdp_send_key(input->session, scancode, down, extended);
    return true;
}

bool native_input_sync_locks(NativeInput *input, bool scroll_lock, bool num_lock, bool caps_lock) {
    if (!native_input_ready(input)) {
        return false;
    }
    rdp_send_sync(input->session, scroll_lock, num_lock, caps_lock);
    return true;
}

bool native_input_linux_keycode_to_rdp(uint32_t code, uint8_t *rdp_scancode, bool *extended) {
    uint8_t scancode = 0;
    bool is_extended = false;
    /* Linux input event codes for the AT-keyboard main block (1..88) ARE the RDP set-1 make
     * codes (KEY_ESC=1=0x01, KEY_A=30=0x1e, KEY_F5=63=0x3f, KEY_KP5=76=0x4c, KEY_102ND=86=
     * 0x56), so those map identity, not extended. The E0-prefixed keys have Linux codes
     * above 88 and need an explicit table. */
    if (code >= 1 && code <= 88) {
        scancode = (uint8_t)code;
    } else {
        switch (code) {
        case 96: /* KEY_KPENTER */
            scancode = 0x1c;
            is_extended = true;
            break;
        case 97: /* KEY_RIGHTCTRL */
            scancode = 0x1d;
            is_extended = true;
            break;
        case 98: /* KEY_KPSLASH */
            scancode = 0x35;
            is_extended = true;
            break;
        case 99: /* KEY_SYSRQ (PrintScreen) */
            scancode = 0x37;
            is_extended = true;
            break;
        case 100: /* KEY_RIGHTALT */
            scancode = 0x38;
            is_extended = true;
            break;
        case 102: /* KEY_HOME */
            scancode = 0x47;
            is_extended = true;
            break;
        case 103: /* KEY_UP */
            scancode = 0x48;
            is_extended = true;
            break;
        case 104: /* KEY_PAGEUP */
            scancode = 0x49;
            is_extended = true;
            break;
        case 105: /* KEY_LEFT */
            scancode = 0x4b;
            is_extended = true;
            break;
        case 106: /* KEY_RIGHT */
            scancode = 0x4d;
            is_extended = true;
            break;
        case 107: /* KEY_END */
            scancode = 0x4f;
            is_extended = true;
            break;
        case 108: /* KEY_DOWN */
            scancode = 0x50;
            is_extended = true;
            break;
        case 109: /* KEY_PAGEDOWN */
            scancode = 0x51;
            is_extended = true;
            break;
        case 110: /* KEY_INSERT */
            scancode = 0x52;
            is_extended = true;
            break;
        case 111: /* KEY_DELETE */
            scancode = 0x53;
            is_extended = true;
            break;
        case 125: /* KEY_LEFTMETA */
            scancode = 0x5b;
            is_extended = true;
            break;
        case 126: /* KEY_RIGHTMETA */
            scancode = 0x5c;
            is_extended = true;
            break;
        case 127: /* KEY_COMPOSE (Menu) */
            scancode = 0x5d;
            is_extended = true;
            break;
        case 352: /* KEY_OK (remote center button) -> main Enter */
            scancode = 0x1c;
            break;
        default:
            clog_limited(cLogLevelTrace, 4, 5000, "no RDP scancode mapping for Linux keycode %u",
                         (unsigned)code);
            return false;
        }
    }
    if (rdp_scancode) {
        *rdp_scancode = scancode;
    }
    if (extended) {
        *extended = is_extended;
    }
    return true;
}
