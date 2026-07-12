#ifndef GNOMECAST_UI_PROFILE_NAME_H
#define GNOMECAST_UI_PROFILE_NAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* These ranges are the intersection of the profile-name requirement and the
 * checked-in IBM Plex 2.005/3.005 sources. Keeping the input policy here aligned
 * with the generated fallback fonts prevents LVGL placeholder boxes. */
static inline bool native_ui_profile_name_codepoint_supported(uint32_t codepoint) {
    return (codepoint >= 0x20u && codepoint <= 0x7eu) ||
           (codepoint >= 0xa0u && codepoint <= 0xffu) ||
           (codepoint >= 0x400u && codepoint <= 0x45fu) ||
           (codepoint >= 0x462u && codepoint <= 0x463u) ||
           (codepoint >= 0x46au && codepoint <= 0x46bu) ||
           (codepoint >= 0x472u && codepoint <= 0x475u) ||
           (codepoint >= 0x490u && codepoint <= 0x4c2u) ||
           (codepoint >= 0x4cfu && codepoint <= 0x4d9u) ||
           (codepoint >= 0x4dcu && codepoint <= 0x4e9u) ||
           (codepoint >= 0x4eeu && codepoint <= 0x4f9u);
}

static inline bool native_ui_profile_name_valid(const char *text, size_t capacity) {
    if (!text || capacity == 0u || strlen(text) >= capacity) {
        return false;
    }
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t codepoint;
        if (*cursor <= 0x7fu) {
            codepoint = *cursor++;
        } else if (*cursor >= 0xc2u && *cursor <= 0xdfu &&
                   cursor[1] >= 0x80u && cursor[1] <= 0xbfu) {
            codepoint = ((uint32_t)(cursor[0] & 0x1fu) << 6) |
                        (uint32_t)(cursor[1] & 0x3fu);
            cursor += 2;
        } else {
            /* No supported profile-name glyph needs a 3- or 4-byte UTF-8 sequence. */
            return false;
        }
        if (!native_ui_profile_name_codepoint_supported(codepoint)) {
            return false;
        }
    }
    return true;
}

static inline const char *native_ui_profile_name_accepted_chars(void) {
    /* 95 ASCII bytes + at most 352 two-byte codepoints + NUL. */
    static char accepted[800];
    static bool initialized;
    if (!initialized) {
        size_t offset = 0;
        for (uint32_t codepoint = 0x20u; codepoint <= 0x4ffu; codepoint++) {
            if (!native_ui_profile_name_codepoint_supported(codepoint)) {
                continue;
            }
            if (codepoint <= 0x7fu) {
                accepted[offset++] = (char)codepoint;
            } else {
                accepted[offset++] = (char)(0xc0u | (codepoint >> 6));
                accepted[offset++] = (char)(0x80u | (codepoint & 0x3fu));
            }
        }
        accepted[offset] = '\0';
        initialized = true;
    }
    return accepted;
}

#endif
