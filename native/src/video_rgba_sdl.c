#include "video_rgba_sdl.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_video, cLogLevelInfo, cLogFlags_Default, "video.rgba", NULL);

#define NATIVE_RGBA_RENDER_COPY_MAX_ATTEMPTS 3u

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
    uint8_t render_copy_failures;
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
        clog(cLogLevelError, "failed to allocate RGBA surface");
        return NULL;
    }
    if (native_rgba_surface_resize(surface, width, height) != NATIVE_RGBA_OK) {
        native_rgba_surface_close(surface);
        return NULL;
    }
    clog(cLogLevelDebug, "opened RGBA surface %ux%u", (unsigned)width, (unsigned)height);
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
    clog(cLogLevelDebug, "closed RGBA surface %ux%u", (unsigned)surface->width, (unsigned)surface->height);
    free(surface->pixels);
    free(surface);
}

NativeRgbaResult native_rgba_surface_resize(NativeRgbaSurface *surface, uint16_t width, uint16_t height) {
    if (!surface) {
        clog_limited(cLogLevelWarning, 2, 5000, "cannot resize NULL RGBA surface");
        return NATIVE_RGBA_INVALID;
    }
    size_t len = 0;
    if (!native_rgba_size(width, height, &len)) {
        clog_limited(cLogLevelWarning, 2, 5000, "invalid RGBA surface size %ux%u", (unsigned)width,
                     (unsigned)height);
        return NATIVE_RGBA_INVALID;
    }
    if (surface->width == width && surface->height == height && surface->pixels) {
        return NATIVE_RGBA_OK;
    }
    uint8_t *pixels = (uint8_t *)calloc(1, len);
    if (!pixels) {
        clog_limited(cLogLevelError, 2, 5000, "failed to allocate %zu bytes for RGBA surface %ux%u", len,
                     (unsigned)width, (unsigned)height);
        return NATIVE_RGBA_NOMEM;
    }
    free(surface->pixels);
    surface->pixels = pixels;
    surface->pixels_len = len;
    surface->width = width;
    surface->height = height;
    surface->has_frame = false;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    surface->render_copy_failures = 0;
    if (surface->texture) {
        SDL_DestroyTexture(surface->texture);
        surface->texture = NULL;
        surface->texture_renderer = NULL;
        surface->texture_width = 0;
        surface->texture_height = 0;
    }
#endif
    clog(cLogLevelDebug, "resized RGBA surface to %ux%u", (unsigned)width, (unsigned)height);
    return NATIVE_RGBA_OK;
}

NativeRgbaResult native_rgba_surface_apply(NativeRgbaSurface *surface, uint32_t left, uint32_t top, uint32_t width,
                                           uint32_t height, uint32_t stride, const uint8_t *rgba, size_t len) {
    /* On 32-bit targets `width * 4` wraps for width >= 2^30, so the row size must be
     * range-checked before any comparison that uses it. */
    if (!surface || !surface->pixels || !rgba || width == 0 || height == 0 || (size_t)width > SIZE_MAX / 4u) {
        clog_limited(cLogLevelWarning, 2, 5000,
                     "rejected invalid RGBA update (surface=%s pixels=%s data=%s rect=%ux%u)",
                     surface ? "set" : "NULL", surface && surface->pixels ? "set" : "NULL",
                     rgba ? "set" : "NULL", (unsigned)width, (unsigned)height);
        return NATIVE_RGBA_INVALID;
    }
    const size_t row_size = (size_t)width * 4u;
    if ((size_t)stride < row_size) {
        clog_limited(cLogLevelWarning, 2, 5000, "rejected RGBA update: stride %u is smaller than row size %zu",
                     (unsigned)stride, row_size);
        return NATIVE_RGBA_INVALID;
    }
    if (left >= surface->width || top >= surface->height) {
        return NATIVE_RGBA_OK;
    }
    if (height > 1 && (size_t)stride > (SIZE_MAX - row_size) / (size_t)(height - 1u)) {
        clog_limited(cLogLevelWarning, 2, 5000, "rejected RGBA update with overflowing dimensions");
        return NATIVE_RGBA_INVALID;
    }
    size_t required = (size_t)stride * (size_t)(height - 1u) + row_size;
    if (len < required) {
        clog_limited(cLogLevelWarning, 2, 5000, "rejected RGBA update: need %zu bytes, received %zu", required,
                     len);
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
static void native_rgba_drop_texture(NativeRgbaSurface *surface) {
    if (!surface) {
        return;
    }
    if (surface->texture) {
        SDL_DestroyTexture(surface->texture);
    }
    surface->texture = NULL;
    surface->texture_renderer = NULL;
    surface->texture_width = 0;
    surface->texture_height = 0;
}

static NativeRgbaResult native_rgba_ensure_texture(NativeRgbaSurface *surface, SDL_Renderer *renderer) {
    if (!surface || !renderer || !surface->pixels) {
        return NATIVE_RGBA_INVALID;
    }
    if (surface->texture && surface->texture_renderer == renderer && surface->texture_width == surface->width &&
        surface->texture_height == surface->height) {
        return NATIVE_RGBA_OK;
    }
    if (surface->texture) {
        native_rgba_drop_texture(surface);
    }
    surface->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, surface->width,
                                         surface->height);
    if (!surface->texture) {
        clog_limited(cLogLevelError, 2, 5000, "failed to create RGBA SDL texture %ux%u: %s",
                     (unsigned)surface->width, (unsigned)surface->height, SDL_GetError());
        return NATIVE_RGBA_UNSUPPORTED;
    }
    surface->texture_renderer = renderer;
    surface->texture_width = surface->width;
    surface->texture_height = surface->height;
    clog(cLogLevelDebug, "created RGBA SDL texture %ux%u", (unsigned)surface->width, (unsigned)surface->height);
    return NATIVE_RGBA_OK;
}

NativeRgbaResult native_rgba_surface_render(NativeRgbaSurface *surface, SDL_Renderer *renderer,
                                            uint16_t viewport_width, uint16_t viewport_height) {
    if (!surface || !renderer || !surface->has_frame) {
        return NATIVE_RGBA_INVALID;
    }
    NativeRgbaResult result = native_rgba_ensure_texture(surface, renderer);
    if (result != NATIVE_RGBA_OK) {
        return result;
    }
    if (SDL_UpdateTexture(surface->texture, NULL, surface->pixels, (int)surface->width * 4) != 0) {
        clog_limited(cLogLevelError, 2, 5000, "failed to update RGBA SDL texture: %s", SDL_GetError());
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
    if (SDL_RenderCopy(renderer, surface->texture, NULL, &dst) != 0) {
        if (surface->render_copy_failures < NATIVE_RGBA_RENDER_COPY_MAX_ATTEMPTS) {
            surface->render_copy_failures++;
        }
        if (surface->render_copy_failures < NATIVE_RGBA_RENDER_COPY_MAX_ATTEMPTS) {
            clog_limited(cLogLevelWarning, 2, 5000,
                         "transient RGBA SDL render failure %u/3; recreating texture: %s",
                         (unsigned)surface->render_copy_failures, SDL_GetError());
            /* A context reset can leave an otherwise valid SDL_Texture unusable. The
             * cached CPU frame survives, so rebuild the texture on the next tick. */
            native_rgba_drop_texture(surface);
            return NATIVE_RGBA_RETRY;
        }
        clog_limited(cLogLevelError, 2, 5000,
                     "failed to render RGBA SDL texture after %u consecutive attempts: %s",
                     (unsigned)surface->render_copy_failures, SDL_GetError());
        native_rgba_drop_texture(surface);
        return NATIVE_RGBA_UNSUPPORTED;
    }
    if (surface->render_copy_failures != 0u) {
        clog(cLogLevelDebug, "RGBA SDL rendering recovered after %u failed attempt%s",
             (unsigned)surface->render_copy_failures,
             surface->render_copy_failures == 1u ? "" : "s");
        surface->render_copy_failures = 0;
    }
    return NATIVE_RGBA_OK;
}

NativeRgbaResult native_rgba_surface_present(NativeRgbaSurface *surface, SDL_Renderer *renderer,
                                             uint16_t viewport_width, uint16_t viewport_height) {
    NativeRgbaResult result = native_rgba_surface_render(surface, renderer, viewport_width, viewport_height);
    if (result == NATIVE_RGBA_OK) {
        SDL_RenderPresent(renderer);
    }
    return result;
}

SDL_Texture *native_rgba_surface_take_texture(NativeRgbaSurface *surface) {
    if (!surface) {
        return NULL;
    }
    /* An explicit renderer-lifecycle detach starts a fresh recovery budget even
     * when a prior failed draw already dropped the texture. */
    surface->render_copy_failures = 0;
    if (!surface->texture) {
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
