#include "video_rgba_sdl.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fake_texture_creates;
static int fake_texture_destroys;
static int fake_render_copy_calls;
static int fake_render_copy_failures;

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, uint32_t format, int access,
                               int width, int height) {
    (void)renderer;
    (void)format;
    (void)access;
    (void)width;
    (void)height;
    fake_texture_creates++;
    return (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
}

void SDL_DestroyTexture(SDL_Texture *texture) {
    if (texture) {
        fake_texture_destroys++;
        free(texture);
    }
}

int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch) {
    (void)texture;
    (void)rect;
    (void)pixels;
    (void)pitch;
    return 0;
}

int SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture) {
    (void)renderer;
    (void)texture;
    return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer *renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    (void)renderer;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    return 0;
}

int SDL_RenderClear(SDL_Renderer *renderer) {
    (void)renderer;
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *src,
                   const SDL_Rect *dst) {
    (void)renderer;
    (void)texture;
    (void)src;
    (void)dst;
    fake_render_copy_calls++;
    if (fake_render_copy_failures > 0) {
        fake_render_copy_failures--;
        return -1;
    }
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *renderer) {
    (void)renderer;
}

const char *SDL_GetError(void) {
    return "injected renderer failure";
}

static void fake_sdl_reset(void) {
    fake_texture_creates = 0;
    fake_texture_destroys = 0;
    fake_render_copy_calls = 0;
    fake_render_copy_failures = 0;
}

static NativeRgbaSurface *surface_with_frame(void) {
    NativeRgbaSurface *surface = native_rgba_surface_open(2, 2);
    uint8_t pixels[2 * 2 * 4] = {0};
    assert(surface);
    assert(native_rgba_surface_apply(surface, 0, 0, 2, 2, 2 * 4,
                                     pixels, sizeof(pixels)) == NATIVE_RGBA_OK);
    return surface;
}

static void test_apply_and_clip(void) {
    NativeRgbaSurface *surface = native_rgba_surface_open(4, 3);
    assert(surface);
    assert(native_rgba_surface_width(surface) == 4);
    assert(native_rgba_surface_height(surface) == 3);
    assert(!native_rgba_surface_has_frame(surface));

    uint8_t pixels[2 * 2 * 4];
    memset(pixels, 0x7f, sizeof(pixels));
    assert(native_rgba_surface_apply(surface, 1, 1, 2, 2, 2 * 4, pixels, sizeof(pixels)) == NATIVE_RGBA_OK);
    assert(native_rgba_surface_has_frame(surface));

    uint8_t edge[2 * 2 * 4];
    memset(edge, 0xa5, sizeof(edge));
    assert(native_rgba_surface_apply(surface, 3, 2, 2, 2, 2 * 4, edge, sizeof(edge)) == NATIVE_RGBA_OK);

    native_rgba_surface_close(surface);
}

static void test_invalid_lengths(void) {
    NativeRgbaSurface *surface = native_rgba_surface_open(4, 4);
    assert(surface);
    uint8_t pixels[16];
    assert(native_rgba_surface_apply(surface, 0, 0, 2, 2, 4, pixels, sizeof(pixels)) == NATIVE_RGBA_INVALID);
    assert(native_rgba_surface_apply(surface, 0, 0, 2, 2, 8, pixels, sizeof(pixels) - 1) == NATIVE_RGBA_INVALID);
    native_rgba_surface_close(surface);
}

static void test_overflowing_dimensions(void) {
    NativeRgbaSurface *surface = native_rgba_surface_open(4, 4);
    assert(surface);
    uint8_t pixels[16];
    /* width * 4 wraps to 0 in 32-bit arithmetic; must not pass the stride/length checks. */
    assert(native_rgba_surface_apply(surface, 0, 0, 0x40000000u, 1, 0, pixels, sizeof(pixels)) ==
           NATIVE_RGBA_INVALID);
    assert(native_rgba_surface_apply(surface, 0, 0, 0x40000000u, 1, 0xFFFFFFFFu, pixels, sizeof(pixels)) ==
           NATIVE_RGBA_INVALID);
    /* stride * (height - 1) wraps in 32-bit size_t; the guard must reject it. */
    assert(native_rgba_surface_apply(surface, 0, 0, 32768u, 32768u, 32768u * 4u, pixels, sizeof(pixels)) ==
           NATIVE_RGBA_INVALID);
    native_rgba_surface_close(surface);
}

static void test_render_copy_recovers_after_one_failure(void) {
    fake_sdl_reset();
    NativeRgbaSurface *surface = surface_with_frame();
    SDL_Renderer renderer = {0};
    fake_render_copy_failures = 1;

    assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_RETRY);
    assert(fake_texture_creates == 1);
    assert(fake_texture_destroys == 1);
    assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_OK);
    assert(fake_texture_creates == 2); /* stale texture was rebuilt from cached pixels */

    native_rgba_surface_close(surface);
    assert(fake_texture_destroys == 2);
}

static void test_render_copy_persistent_failure_is_terminal(void) {
    fake_sdl_reset();
    NativeRgbaSurface *surface = surface_with_frame();
    SDL_Renderer renderer = {0};
    fake_render_copy_failures = 3;

    assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_RETRY);
    assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_RETRY);
    assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_UNSUPPORTED);
    assert(fake_render_copy_calls == 3);
    assert(fake_texture_creates == 3);
    assert(fake_texture_destroys == 3);

    native_rgba_surface_close(surface);
}

static void test_success_resets_render_failure_streak(void) {
    fake_sdl_reset();
    NativeRgbaSurface *surface = surface_with_frame();
    SDL_Renderer renderer = {0};

    for (int streak = 0; streak < 2; streak++) {
        fake_render_copy_failures = 2;
        assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_RETRY);
        assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_RETRY);
        assert(native_rgba_surface_render(surface, &renderer, 2, 2) == NATIVE_RGBA_OK);
    }

    native_rgba_surface_close(surface);
}

int main(void) {
    test_apply_and_clip();
    test_invalid_lengths();
    test_overflowing_dimensions();
    test_render_copy_recovers_after_one_failure();
    test_render_copy_persistent_failure_is_terminal();
    test_success_resets_render_failure_streak();
    return 0;
}
