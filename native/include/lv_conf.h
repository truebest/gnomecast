#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <stdlib.h>

#define LV_COLOR_DEPTH 32
#define LV_COLOR_SCREEN_TRANSP 1
#define LV_GRADIENT_OPACITY 1

#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_MEMCPY_MEMSET_STD 1

#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
#define LV_TICK_CUSTOM_INCLUDE <SDL.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (SDL_GetTicks())
#endif

#define LV_DPI_DEF 320
#define LV_DISP_DEF_REFR_PERIOD 17
#define LV_INDEV_DEF_READ_PERIOD 1

#define LV_USE_GPU_SDL 1
#define LV_GPU_SDL_INCLUDE_PATH <SDL.h>
#define LV_GPU_SDL_CUSTOM_BLEND_MODE 0

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

#define LV_SPRINTF_CUSTOM 1
#if LV_SPRINTF_CUSTOM
#define LV_SPRINTF_INCLUDE <stdio.h>
#define lv_snprintf snprintf
#define lv_vsnprintf vsnprintf
#endif

#define LV_USE_USER_DATA 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

#endif
