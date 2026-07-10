#include "cursor_sdl.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_scale_rgba_area_average(void) {
    /* Upscale 2x2 distinct opaque pixels -> 4x4: each source pixel covers a 2x2 block. */
    const uint8_t src[2 * 2 * 4] = {
        0x11, 0x11, 0x11, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x44, 0x44, 0x44, 0xff,
    };
    uint8_t dst[4 * 4 * 4];
    assert(native_cursor_scale_rgba(src, 2, 2, dst, 4, 4));
    assert(dst[0] == 0x11);               /* (0,0) */
    assert(dst[1 * 4] == 0x11);           /* (1,0) still inside the first source pixel */
    assert(dst[2 * 4] == 0x22);           /* (2,0) second source pixel */
    assert(dst[(4 * 2 + 0) * 4] == 0x33); /* (0,2) third source pixel */
    assert(dst[(4 * 3 + 3) * 4] == 0x44); /* (3,3) fourth source pixel */

    /* Downscale 4x4 -> 2x2 averages each 2x2 block exactly. */
    uint8_t dst2[2 * 2 * 4];
    assert(native_cursor_scale_rgba(dst, 4, 4, dst2, 2, 2));
    assert(dst2[0] == 0x11 && dst2[1 * 4] == 0x22 && dst2[2 * 4] == 0x33 && dst2[3 * 4] == 0x44);

    /* Downscale of an opaque gradient block averages the values: (0x10+0x20+0x30+0x40)/4. */
    const uint8_t grad[2 * 2 * 4] = {
        0x10, 0x00, 0x00, 0xff, 0x20, 0x00, 0x00, 0xff,
        0x30, 0x00, 0x00, 0xff, 0x40, 0x00, 0x00, 0xff,
    };
    uint8_t one[4];
    assert(native_cursor_scale_rgba(grad, 2, 2, one, 1, 1));
    assert(one[0] == 0x28 && one[1] == 0x00 && one[2] == 0x00 && one[3] == 0xff);

    /* Alpha-weighted averaging: opaque red + fully transparent black must stay pure red
     * (no dark edge fringe); alpha itself averages to half. */
    const uint8_t edge[2 * 1 * 4] = {
        0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
    };
    assert(native_cursor_scale_rgba(edge, 2, 1, one, 1, 1));
    assert(one[0] == 0xff && one[1] == 0x00 && one[2] == 0x00);
    assert(one[3] == 0x80);

    /* Fully transparent input stays fully transparent zeroed pixels. */
    const uint8_t clear[2 * 1 * 4] = {0};
    assert(native_cursor_scale_rgba(clear, 2, 1, one, 1, 1));
    assert(one[0] == 0 && one[1] == 0 && one[2] == 0 && one[3] == 0);

    assert(!native_cursor_scale_rgba(NULL, 2, 2, dst, 4, 4));
    assert(!native_cursor_scale_rgba(src, 0, 2, dst, 4, 4));
    assert(!native_cursor_scale_rgba(src, 2, 2, dst, 0, 4));
}

static void test_scaled_geometry(void) {
    uint16_t w = 0, h = 0, hx = 0, hy = 0;

    /* 4K desktop onto a 1080p window: everything halves. */
    native_cursor_scaled_geometry(64, 64, 32, 16, 3840, 2160, 1920, 1080, &w, &h, &hx, &hy);
    assert(w == 32 && h == 32 && hx == 16 && hy == 8);

    /* Equal sizes: 1:1. */
    native_cursor_scaled_geometry(32, 32, 5, 7, 1920, 1080, 1920, 1080, &w, &h, &hx, &hy);
    assert(w == 32 && h == 32 && hx == 5 && hy == 7);

    /* Unknown desktop: 1:1. */
    native_cursor_scaled_geometry(32, 32, 5, 7, 0, 0, 1920, 1080, &w, &h, &hx, &hy);
    assert(w == 32 && h == 32 && hx == 5 && hy == 7);

    /* Tiny shape never scales below 1x1 and the hotspot stays inside. */
    native_cursor_scaled_geometry(1, 1, 0, 0, 3840, 2160, 1920, 1080, &w, &h, &hx, &hy);
    assert(w == 1 && h == 1 && hx == 0 && hy == 0);

    /* Hotspot clamped inside the scaled shape. */
    native_cursor_scaled_geometry(2, 2, 1, 1, 3840, 2160, 1920, 1080, &w, &h, &hx, &hy);
    assert(w == 1 && h == 1 && hx == 0 && hy == 0);

    /* A hostile/degenerate tiny desktop must not balloon a legal shape past the cap:
     * 384px over a 16x9 "desktop" would map to 46080px, whose w*h*4 allocation size
     * overflows 32-bit size_t. Hotspots are RESCALED by the cap ratio, not clamped to
     * the edge: 45960/46080 of the width stays 1021/1024, not 1023. */
    native_cursor_scaled_geometry(NATIVE_CURSOR_MAX_DIM, NATIVE_CURSOR_MAX_DIM, NATIVE_CURSOR_MAX_DIM - 1,
                                  NATIVE_CURSOR_MAX_DIM - 1, 16, 9, 1920, 1080, &w, &h, &hx, &hy);
    assert(w == NATIVE_CURSOR_MAX_SCALED_DIM && h == NATIVE_CURSOR_MAX_SCALED_DIM);
    assert(hx == 1021 && hy == 1021);
    assert((size_t)w * (size_t)h * 4u <= 4u * 1024u * 1024u); /* alloc stays bounded */

    /* A centered hotspot stays centered through the cap (192/384 -> 512/1024). */
    native_cursor_scaled_geometry(NATIVE_CURSOR_MAX_DIM, NATIVE_CURSOR_MAX_DIM, 192, 192, 16, 9, 1920, 1080, &w, &h,
                                  &hx, &hy);
    assert(w == NATIVE_CURSOR_MAX_SCALED_DIM && h == NATIVE_CURSOR_MAX_SCALED_DIM);
    assert(hx == 512 && hy == 512);

    /* Extreme ratio saturating cursor_scale_dim (1px "desktop"): both the mapped size
     * and hotspot hit UINT16_MAX, so the capped hotspot must come from the ORIGINAL
     * shape geometry — 192/384 is still the center, not the far edge. */
    native_cursor_scaled_geometry(NATIVE_CURSOR_MAX_DIM, NATIVE_CURSOR_MAX_DIM, 192, 192, 1, 1, 1920, 1080, &w, &h,
                                  &hx, &hy);
    assert(w == NATIVE_CURSOR_MAX_SCALED_DIM && h == NATIVE_CURSOR_MAX_SCALED_DIM);
    assert(hx == 512 && hy == 512);
}

static void test_submit_state_machine(void) {
    NativeCursor cursor;
    native_cursor_init(&cursor);
    assert(cursor.desired == NATIVE_CURSOR_DEFAULT);
    assert(atomic_load(&cursor.generation) == 0u);

    uint8_t shape[4 * 4 * 4];
    memset(shape, 0xaa, sizeof(shape));

    /* Invalid submissions are dropped: len mismatch, zero/oversized dims. */
    native_cursor_submit_bitmap(&cursor, 4, 4, 0, 0, shape, sizeof(shape) - 1);
    native_cursor_submit_bitmap(&cursor, 0, 4, 0, 0, shape, 0);
    native_cursor_submit_bitmap(&cursor, NATIVE_CURSOR_MAX_DIM + 1, 1, 0, 0, shape,
                                (size_t)(NATIVE_CURSOR_MAX_DIM + 1) * 4u);
    native_cursor_submit_state(&cursor, 99u);
    assert(atomic_load(&cursor.generation) == 0u);
    assert(cursor.shape_rgba == NULL);

    /* A valid shape lands and flips desired. */
    native_cursor_submit_bitmap(&cursor, 4, 4, 1, 2, shape, sizeof(shape));
    assert(atomic_load(&cursor.generation) == 1u);
    assert(cursor.desired == NATIVE_CURSOR_SHAPE);
    assert(cursor.shape_rgba != NULL);
    assert(cursor.shape_width == 4 && cursor.shape_height == 4);
    assert(cursor.hotspot_x == 1 && cursor.hotspot_y == 2);
    assert(memcmp(cursor.shape_rgba, shape, sizeof(shape)) == 0);

    /* A newer shape replaces the pending one. */
    uint8_t shape2[2 * 2 * 4];
    memset(shape2, 0xbb, sizeof(shape2));
    native_cursor_submit_bitmap(&cursor, 2, 2, 0, 0, shape2, sizeof(shape2));
    assert(atomic_load(&cursor.generation) == 2u);
    assert(cursor.shape_width == 2 && memcmp(cursor.shape_rgba, shape2, sizeof(shape2)) == 0);

    /* Hidden/default win as the latest event; the pending shape buffer stays for SHAPE. */
    native_cursor_submit_state(&cursor, NATIVE_CURSOR_HIDDEN);
    assert(cursor.desired == NATIVE_CURSOR_HIDDEN);
    native_cursor_submit_state(&cursor, NATIVE_CURSOR_DEFAULT);
    assert(cursor.desired == NATIVE_CURSOR_DEFAULT);
    assert(atomic_load(&cursor.generation) == 4u);

    native_cursor_destroy(&cursor);
}

int main(void) {
    test_scale_rgba_area_average();
    test_scaled_geometry();
    test_submit_state_machine();
    return 0;
}
