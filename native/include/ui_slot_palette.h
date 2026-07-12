#ifndef GNOMECAST_UI_SLOT_PALETTE_H
#define GNOMECAST_UI_SLOT_PALETTE_H

#include <stdint.h>

#include "settings_json.h"

/* Shared remote-button identity colors. Keep this header independent of SDL/LVGL so
 * streaming overlays and both native UI modules cannot drift onto separate palettes. */
enum NativeUiSlotRgb {
    NATIVE_UI_SLOT_RED_RGB = 0xe94646,
    NATIVE_UI_SLOT_GREEN_RGB = 0x37bb62,
    NATIVE_UI_SLOT_YELLOW_RGB = 0xe3b831,
    NATIVE_UI_SLOT_BLUE_RGB = 0x3f8dea,
};

static inline uint32_t native_ui_slot_rgb(int slot) {
    switch (slot) {
    case NATIVE_SESSION_SLOT_RED:
        return NATIVE_UI_SLOT_RED_RGB;
    case NATIVE_SESSION_SLOT_GREEN:
        return NATIVE_UI_SLOT_GREEN_RGB;
    case NATIVE_SESSION_SLOT_YELLOW:
        return NATIVE_UI_SLOT_YELLOW_RGB;
    case NATIVE_SESSION_SLOT_BLUE:
    default:
        return NATIVE_UI_SLOT_BLUE_RGB;
    }
}

#endif
