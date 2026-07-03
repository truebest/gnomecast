#include "input_sdl.h"

#include <stddef.h>
#include <string.h>

static uint16_t clamp_dimension(uint16_t value) {
    return value == 0 ? 1 : value;
}

static bool native_input_ready(const NativeInput *input) {
    return input && atomic_load(&input->active) && input->session;
}

void native_input_init(NativeInput *input, RdpSession *session, uint16_t desktop_width, uint16_t desktop_height) {
    if (!input) {
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
}

void native_input_set_session(NativeInput *input, RdpSession *session) {
    if (!input) {
        return;
    }
    input->session = session;
}

void native_input_set_active(NativeInput *input, bool active) {
    if (!input) {
        return;
    }
    atomic_store(&input->active, active);
}

bool native_input_is_active(const NativeInput *input) {
    return input && atomic_load(&input->active);
}

void native_input_set_desktop_size(NativeInput *input, uint16_t desktop_width, uint16_t desktop_height) {
    if (!input) {
        return;
    }
    atomic_store(&input->desktop_width, clamp_dimension(desktop_width));
    atomic_store(&input->desktop_height, clamp_dimension(desktop_height));
}

void native_input_set_window_size(NativeInput *input, uint16_t window_width, uint16_t window_height) {
    if (!input) {
        return;
    }
    atomic_store(&input->window_width, clamp_dimension(window_width));
    atomic_store(&input->window_height, clamp_dimension(window_height));
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

bool native_input_unicode(NativeInput *input, uint16_t codepoint, bool down) {
    if (!native_input_ready(input)) {
        return false;
    }
    rdp_send_unicode(input->session, codepoint, down);
    return true;
}

bool native_input_sdl_scancode_to_rdp(uint32_t sdl_scancode, uint8_t *rdp_scancode, bool *extended) {
    uint8_t scancode = 0;
    bool is_extended = false;

    static const uint8_t letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    };
    static const uint8_t numbers[10] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    };
    static const uint8_t keypad_digits[10] = {
        0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49,
    };

    if (sdl_scancode >= 4 && sdl_scancode <= 29) {
        scancode = letters[sdl_scancode - 4];
    } else if (sdl_scancode >= 30 && sdl_scancode <= 39) {
        scancode = numbers[sdl_scancode - 30];
    } else if (sdl_scancode >= 58 && sdl_scancode <= 67) {
        scancode = (uint8_t)(0x3b + (sdl_scancode - 58));
    } else if (sdl_scancode >= 89 && sdl_scancode <= 98) {
        scancode = keypad_digits[sdl_scancode - 89];
    } else {
        switch (sdl_scancode) {
        case 40: /* Return */
            scancode = 0x1c;
            break;
        case 41: /* Escape */
            scancode = 0x01;
            break;
        case 42: /* Backspace */
            scancode = 0x0e;
            break;
        case 43: /* Tab */
            scancode = 0x0f;
            break;
        case 44: /* Space */
            scancode = 0x39;
            break;
        case 45: /* Minus */
            scancode = 0x0c;
            break;
        case 46: /* Equals */
            scancode = 0x0d;
            break;
        case 47: /* Left bracket */
            scancode = 0x1a;
            break;
        case 48: /* Right bracket */
            scancode = 0x1b;
            break;
        case 49: /* Backslash */
        case 50: /* Non-US hash */
            scancode = 0x2b;
            break;
        case 51: /* Semicolon */
            scancode = 0x27;
            break;
        case 52: /* Apostrophe */
            scancode = 0x28;
            break;
        case 53: /* Grave */
            scancode = 0x29;
            break;
        case 54: /* Comma */
            scancode = 0x33;
            break;
        case 55: /* Period */
            scancode = 0x34;
            break;
        case 56: /* Slash */
            scancode = 0x35;
            break;
        case 57: /* Caps Lock */
            scancode = 0x3a;
            break;
        case 68: /* F11 */
            scancode = 0x57;
            break;
        case 69: /* F12 */
            scancode = 0x58;
            break;
        case 70: /* Print Screen */
            scancode = 0x37;
            is_extended = true;
            break;
        case 71: /* Scroll Lock */
            scancode = 0x46;
            break;
        case 73: /* Insert */
            scancode = 0x52;
            is_extended = true;
            break;
        case 74: /* Home */
            scancode = 0x47;
            is_extended = true;
            break;
        case 75: /* Page Up */
            scancode = 0x49;
            is_extended = true;
            break;
        case 76: /* Delete */
            scancode = 0x53;
            is_extended = true;
            break;
        case 77: /* End */
            scancode = 0x4f;
            is_extended = true;
            break;
        case 78: /* Page Down */
            scancode = 0x51;
            is_extended = true;
            break;
        case 79: /* Right */
            scancode = 0x4d;
            is_extended = true;
            break;
        case 80: /* Left */
            scancode = 0x4b;
            is_extended = true;
            break;
        case 81: /* Down */
            scancode = 0x50;
            is_extended = true;
            break;
        case 82: /* Up */
            scancode = 0x48;
            is_extended = true;
            break;
        case 83: /* Num Lock */
            scancode = 0x45;
            break;
        case 84: /* Keypad divide */
            scancode = 0x35;
            is_extended = true;
            break;
        case 85: /* Keypad multiply */
            scancode = 0x37;
            break;
        case 86: /* Keypad minus */
            scancode = 0x4a;
            break;
        case 87: /* Keypad plus */
            scancode = 0x4e;
            break;
        case 88: /* Keypad enter */
            scancode = 0x1c;
            is_extended = true;
            break;
        case 99: /* Keypad period */
            scancode = 0x53;
            break;
        case 101: /* Application/Menu */
            scancode = 0x5d;
            is_extended = true;
            break;
        case 224: /* Left Ctrl */
            scancode = 0x1d;
            break;
        case 225: /* Left Shift */
            scancode = 0x2a;
            break;
        case 226: /* Left Alt */
            scancode = 0x38;
            break;
        case 227: /* Left GUI */
            scancode = 0x5b;
            is_extended = true;
            break;
        case 228: /* Right Ctrl */
            scancode = 0x1d;
            is_extended = true;
            break;
        case 229: /* Right Shift */
            scancode = 0x36;
            break;
        case 230: /* Right Alt */
            scancode = 0x38;
            is_extended = true;
            break;
        case 231: /* Right GUI */
            scancode = 0x5c;
            is_extended = true;
            break;
        default:
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

static bool native_input_decode_utf8(const unsigned char **cursor, uint32_t *codepoint) {
    const unsigned char *p = *cursor;
    uint32_t cp = 0;
    size_t len = 0;

    if (p[0] < 0x80) {
        cp = p[0];
        len = 1;
    } else if ((p[0] & 0xe0) == 0xc0) {
        cp = p[0] & 0x1f;
        len = 2;
        if (cp == 0) {
            return false;
        }
    } else if ((p[0] & 0xf0) == 0xe0) {
        cp = p[0] & 0x0f;
        len = 3;
    } else if ((p[0] & 0xf8) == 0xf0) {
        cp = p[0] & 0x07;
        len = 4;
    } else {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3f);
    }

    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
        return false;
    }

    *cursor = p + len;
    *codepoint = cp;
    return true;
}

static bool native_input_send_utf16_unit(NativeInput *input, uint16_t unit) {
    bool sent = native_input_unicode(input, unit, true);
    sent = native_input_unicode(input, unit, false) || sent;
    return sent;
}

bool native_input_text_utf8(NativeInput *input, const char *utf8) {
    if (!native_input_ready(input) || !utf8) {
        return false;
    }

    bool sent = false;
    const unsigned char *cursor = (const unsigned char *)utf8;
    while (*cursor) {
        uint32_t cp = 0;
        const unsigned char *before = cursor;
        if (!native_input_decode_utf8(&cursor, &cp)) {
            cursor = before + 1;
            continue;
        }

        if (cp <= 0xffff) {
            sent = native_input_send_utf16_unit(input, (uint16_t)cp) || sent;
        } else {
            cp -= 0x10000;
            sent = native_input_send_utf16_unit(input, (uint16_t)(0xd800 | (cp >> 10))) || sent;
            sent = native_input_send_utf16_unit(input, (uint16_t)(0xdc00 | (cp & 0x3ff))) || sent;
        }
    }
    return sent;
}
