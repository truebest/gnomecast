#include "video_rgba_sdl.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct NativeRgbaSurface {
    uint16_t width;
    uint16_t height;
    uint8_t *pixels;
    size_t pixels_len;
    bool has_frame;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    SDL_Texture *texture;
    SDL_Renderer *texture_renderer;
    uint16_t texture_width;
    uint16_t texture_height;
#endif
};

static bool native_rgba_size(uint16_t width, uint16_t height, size_t *len) {
    if (width == 0 || height == 0 || !len) {
        return false;
    }
    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / 4u) {
        return false;
    }
    *len = w * h * 4u;
    return true;
}

NativeRgbaSurface *native_rgba_surface_open(uint16_t width, uint16_t height) {
    NativeRgbaSurface *surface = (NativeRgbaSurface *)calloc(1, sizeof(NativeRgbaSurface));
    if (!surface) {
        return NULL;
    }
    if (native_rgba_surface_resize(surface, width, height) != NATIVE_RGBA_OK) {
        native_rgba_surface_close(surface);
        return NULL;
    }
    return surface;
}

void native_rgba_surface_close(NativeRgbaSurface *surface) {
    if (!surface) {
        return;
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    if (surface->texture) {
        SDL_DestroyTexture(surface->texture);
    }
#endif
    free(surface->pixels);
    free(surface);
}

NativeRgbaResult native_rgba_surface_resize(NativeRgbaSurface *surface, uint16_t width, uint16_t height) {
    if (!surface) {
        return NATIVE_RGBA_INVALID;
    }
    size_t len = 0;
    if (!native_rgba_size(width, height, &len)) {
        return NATIVE_RGBA_INVALID;
    }
    if (surface->width == width && surface->height == height && surface->pixels) {
        return NATIVE_RGBA_OK;
    }
    uint8_t *pixels = (uint8_t *)calloc(1, len);
    if (!pixels) {
        return NATIVE_RGBA_NOMEM;
    }
    free(surface->pixels);
    surface->pixels = pixels;
    surface->pixels_len = len;
    surface->width = width;
    surface->height = height;
    surface->has_frame = false;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    if (surface->texture) {
        SDL_DestroyTexture(surface->texture);
        surface->texture = NULL;
        surface->texture_renderer = NULL;
        surface->texture_width = 0;
        surface->texture_height = 0;
    }
#endif
    return NATIVE_RGBA_OK;
}

NativeRgbaResult native_rgba_surface_apply(NativeRgbaSurface *surface, uint32_t left, uint32_t top, uint32_t width,
                                           uint32_t height, uint32_t stride, const uint8_t *rgba, size_t len) {
    /* On 32-bit targets `width * 4` wraps for width >= 2^30, so the row size must be
     * range-checked before any comparison that uses it. */
    if (!surface || !surface->pixels || !rgba || width == 0 || height == 0 || (size_t)width > SIZE_MAX / 4u) {
        return NATIVE_RGBA_INVALID;
    }
    const size_t row_size = (size_t)width * 4u;
    if ((size_t)stride < row_size) {
        return NATIVE_RGBA_INVALID;
    }
    if (left >= surface->width || top >= surface->height) {
        return NATIVE_RGBA_OK;
    }
    if (height > 1 && (size_t)stride > (SIZE_MAX - row_size) / (size_t)(height - 1u)) {
        return NATIVE_RGBA_INVALID;
    }
    size_t required = (size_t)stride * (size_t)(height - 1u) + row_size;
    if (len < required) {
        return NATIVE_RGBA_INVALID;
    }

    uint32_t copy_width = width;
    uint32_t copy_height = height;
    if (left + copy_width > surface->width) {
        copy_width = (uint32_t)surface->width - left;
    }
    if (top + copy_height > surface->height) {
        copy_height = (uint32_t)surface->height - top;
    }
    const size_t row_bytes = (size_t)copy_width * 4u;
    for (uint32_t row = 0; row < copy_height; row++) {
        const uint8_t *src = rgba + (size_t)row * (size_t)stride;
        uint8_t *dst = surface->pixels + (((size_t)top + row) * (size_t)surface->width + (size_t)left) * 4u;
        memcpy(dst, src, row_bytes);
    }
    surface->has_frame = true;
    return NATIVE_RGBA_OK;
}

uint16_t native_rgba_surface_width(const NativeRgbaSurface *surface) {
    return surface ? surface->width : 0;
}

uint16_t native_rgba_surface_height(const NativeRgbaSurface *surface) {
    return surface ? surface->height : 0;
}

int native_rgba_surface_has_frame(const NativeRgbaSurface *surface) {
    return surface && surface->has_frame ? 1 : 0;
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
static NativeRgbaResult native_rgba_ensure_texture(NativeRgbaSurface *surface, SDL_Renderer *renderer) {
    if (!surface || !renderer || !surface->pixels) {
        return NATIVE_RGBA_INVALID;
    }
    if (surface->texture && surface->texture_renderer == renderer && surface->texture_width == surface->width &&
        surface->texture_height == surface->height) {
        return NATIVE_RGBA_OK;
    }
    if (surface->texture) {
        SDL_DestroyTexture(surface->texture);
        surface->texture = NULL;
    }
    surface->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, surface->width,
                                         surface->height);
    if (!surface->texture) {
        return NATIVE_RGBA_UNSUPPORTED;
    }
    surface->texture_renderer = renderer;
    surface->texture_width = surface->width;
    surface->texture_height = surface->height;
    return NATIVE_RGBA_OK;
}

NativeRgbaResult native_rgba_surface_present(NativeRgbaSurface *surface, SDL_Renderer *renderer, uint16_t viewport_width,
                                             uint16_t viewport_height) {
    if (!surface || !renderer || !surface->has_frame) {
        return NATIVE_RGBA_INVALID;
    }
    NativeRgbaResult result = native_rgba_ensure_texture(surface, renderer);
    if (result != NATIVE_RGBA_OK) {
        return result;
    }
    if (SDL_UpdateTexture(surface->texture, NULL, surface->pixels, (int)surface->width * 4) != 0) {
        return NATIVE_RGBA_UNSUPPORTED;
    }
    SDL_Rect dst = {0, 0, viewport_width ? (int)viewport_width : (int)surface->width,
                    viewport_height ? (int)viewport_height : (int)surface->height};
    /* The renderer's target may still point at the preconnect UI's offscreen LVGL texture
     * (see ui_preconnect.c), so it must be reset to the window before drawing here, or this
     * frame silently renders into that hidden texture instead of appearing on screen. */
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, surface->texture, NULL, &dst);
    SDL_RenderPresent(renderer);
    return NATIVE_RGBA_OK;
}

SDL_Texture *native_rgba_surface_take_texture(NativeRgbaSurface *surface) {
    if (!surface || !surface->texture) {
        return NULL;
    }
    SDL_Texture *texture = surface->texture;
    surface->texture = NULL;
    surface->texture_renderer = NULL;
    surface->texture_width = 0;
    surface->texture_height = 0;
    return texture;
}
#endif
