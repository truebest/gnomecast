#ifndef GNOMECAST_VIDEO_RGBA_SDL_H
#define GNOMECAST_VIDEO_RGBA_SDL_H

#include <stddef.h>
#include <stdint.h>

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
#include <SDL.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NativeRgbaSurface NativeRgbaSurface;

typedef enum NativeRgbaResult {
    NATIVE_RGBA_OK = 0,
    NATIVE_RGBA_INVALID = 1,
    NATIVE_RGBA_NOMEM = 2,
    NATIVE_RGBA_UNSUPPORTED = 3,
    /* The cached frame is still valid, but this draw was skipped while the SDL
     * texture is recreated after a transient renderer failure. */
    NATIVE_RGBA_RETRY = 4
} NativeRgbaResult;

NativeRgbaSurface *native_rgba_surface_open(uint16_t width, uint16_t height);
void native_rgba_surface_close(NativeRgbaSurface *surface);
NativeRgbaResult native_rgba_surface_resize(NativeRgbaSurface *surface, uint16_t width, uint16_t height);
NativeRgbaResult native_rgba_surface_apply(NativeRgbaSurface *surface, uint32_t left, uint32_t top, uint32_t width,
                                           uint32_t height, uint32_t stride, const uint8_t *rgba, size_t len);
uint16_t native_rgba_surface_width(const NativeRgbaSurface *surface);
uint16_t native_rgba_surface_height(const NativeRgbaSurface *surface);
int native_rgba_surface_has_frame(const NativeRgbaSurface *surface);

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
/* Draws the cached frame into the renderer's window backbuffer without presenting it.
 * Callers can composite translucent UI in the same backbuffer, then present once. */
NativeRgbaResult native_rgba_surface_render(NativeRgbaSurface *surface, SDL_Renderer *renderer,
                                            uint16_t viewport_width, uint16_t viewport_height);
NativeRgbaResult native_rgba_surface_present(NativeRgbaSurface *surface, SDL_Renderer *renderer, uint16_t viewport_width,
                                             uint16_t viewport_height);
/* Detaches and returns the surface's SDL texture (if any) without calling any SDL API,
 * clearing the surface's texture fields. Lets a caller on a thread that doesn't own the
 * renderer hand the texture off to the owning thread for destruction, instead of letting
 * native_rgba_surface_close()/resize() destroy it in place. */
SDL_Texture *native_rgba_surface_take_texture(NativeRgbaSurface *surface);
#endif

#ifdef __cplusplus
}
#endif

#endif
