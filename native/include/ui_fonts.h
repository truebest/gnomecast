#ifndef GNOMECAST_UI_FONTS_H
#define GNOMECAST_UI_FONTS_H

#include "lvgl.h"

/* The complete ten-token TV type scale. Generated C bitmaps keep runtime font
 * loading out of the webOS process; profile-name tokens link Latin-1/Cyrillic
 * fallback bitmaps internally, while icon glyphs remain on LVGL's built-in font. */
LV_FONT_DECLARE(lv_font_ibm_plex_sans_semibold_86);
LV_FONT_DECLARE(lv_font_ibm_plex_sans_semibold_40);
LV_FONT_DECLARE(lv_font_ibm_plex_sans_semibold_24);
LV_FONT_DECLARE(lv_font_ibm_plex_sans_regular_24);
LV_FONT_DECLARE(lv_font_ibm_plex_sans_regular_20);
LV_FONT_DECLARE(lv_font_ibm_plex_mono_semibold_28);
LV_FONT_DECLARE(lv_font_ibm_plex_mono_semibold_18);
LV_FONT_DECLARE(lv_font_ibm_plex_mono_regular_22);
LV_FONT_DECLARE(lv_font_ibm_plex_mono_regular_16);
LV_FONT_DECLARE(lv_font_jetbrains_mono_semibold_28);

#endif
