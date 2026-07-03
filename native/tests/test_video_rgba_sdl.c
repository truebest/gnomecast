#include "video_rgba_sdl.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

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

int main(void) {
    test_apply_and_clip();
    test_invalid_lengths();
    return 0;
}
