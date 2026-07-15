#include "cursor_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdp_ffi.h"

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
#if defined(__has_include)
#if __has_include(<SDL_webOS.h>)
#include <SDL_webOS.h>
#define HELLOLG_CURSOR_HAVE_WEBOS_VISIBILITY 1
#endif
#endif
#endif
#ifndef HELLOLG_CURSOR_HAVE_WEBOS_VISIBILITY
#define HELLOLG_CURSOR_HAVE_WEBOS_VISIBILITY 0
#endif

#include "clog.h"

clog_define(g_native_log_cursor, cLogLevelInfo, cLogFlags_Default, "cursor", NULL);

_Static_assert(NATIVE_CURSOR_HIDDEN == RDP_POINTER_STATE_HIDDEN,
               "NATIVE_CURSOR_HIDDEN must match the FFI constant");
_Static_assert(NATIVE_CURSOR_DEFAULT == RDP_POINTER_STATE_DEFAULT,
               "NATIVE_CURSOR_DEFAULT must match the FFI constant");

void native_cursor_init(NativeCursor *cursor) {
    if (!cursor) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
    pthread_mutex_init(&cursor->lock, NULL);
    cursor->desired = NATIVE_CURSOR_DEFAULT;
    atomic_init(&cursor->generation, 0u);
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    /* The platform pointer starts visible; without this the first server-driven hide would
     * be skipped by the "already hidden" short-circuit and the pointer would stay on screen
     * while the server considers it hidden. */
    cursor->visible = true;
#endif
}

void native_cursor_destroy(NativeCursor *cursor) {
    if (!cursor) {
        return;
    }
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    if (cursor->cursor) {
        SDL_FreeCursor(cursor->cursor);
        cursor->cursor = NULL;
    }
#endif
    pthread_mutex_lock(&cursor->lock);
    free(cursor->shape_rgba);
    cursor->shape_rgba = NULL;
    pthread_mutex_unlock(&cursor->lock);
    pthread_mutex_destroy(&cursor->lock);
}

void native_cursor_submit_bitmap(NativeCursor *cursor, uint16_t width, uint16_t height,
                                 uint16_t hotspot_x, uint16_t hotspot_y, const uint8_t *rgba,
                                 size_t len) {
    if (!cursor) {
        return;
    }
    size_t expected_len = (size_t)width * (size_t)height * 4u;
    if (!rgba || width == 0 || height == 0 || width > NATIVE_CURSOR_MAX_DIM ||
        height > NATIVE_CURSOR_MAX_DIM || len != expected_len) {
        clog_limited(cLogLevelWarning, 4, 5000,
                     "rejected server cursor bitmap %ux%u len=%zu expected=%zu",
                     (unsigned)width, (unsigned)height, len, expected_len);
        return;
    }
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        clog_limited(cLogLevelWarning, 4, 5000,
                     "failed to copy server cursor bitmap %ux%u (%zu bytes)",
                     (unsigned)width, (unsigned)height, len);
        return;
    }
    memcpy(copy, rgba, len);
    pthread_mutex_lock(&cursor->lock);
    uint32_t previous = cursor->desired;
    free(cursor->shape_rgba);
    cursor->shape_rgba = copy;
    cursor->shape_width = width;
    cursor->shape_height = height;
    cursor->hotspot_x = hotspot_x;
    cursor->hotspot_y = hotspot_y;
    cursor->desired = NATIVE_CURSOR_SHAPE;
    cursor->shape_serial++;
    pthread_mutex_unlock(&cursor->lock);
    atomic_fetch_add(&cursor->generation, 1u);
    if (previous != NATIVE_CURSOR_SHAPE) {
        clog(cLogLevelDebug, "server pointer shape queued after state=%u (%ux%u)",
             (unsigned)previous, (unsigned)width, (unsigned)height);
    }
}

void native_cursor_submit_state(NativeCursor *cursor, uint32_t state) {
    if (!cursor || (state != NATIVE_CURSOR_HIDDEN && state != NATIVE_CURSOR_DEFAULT)) {
        return;
    }
    pthread_mutex_lock(&cursor->lock);
    uint32_t previous = cursor->desired;
    cursor->desired = state;
    pthread_mutex_unlock(&cursor->lock);
    atomic_fetch_add(&cursor->generation, 1u);
    if (state != previous) {
        clog(cLogLevelDebug, "server pointer state queued: %s (previous=%u)",
             state == NATIVE_CURSOR_HIDDEN ? "hidden" : "default", (unsigned)previous);
    }
}

/* Area-average resample with alpha-weighted (premultiplied) accumulation. Every source
 * pixel contributes proportionally to its overlap with the destination pixel's footprint,
 * which both antialiases downscaled edges and acts as bilinear-ish filtering on upscale.
 * Color channels are weighted by alpha so transparent pixels (RGB zeroed by the decoder)
 * cannot darken the visible edge — the classic fringe artifact of naive averaging.
 * Runs only on cursor-shape changes (<= 384x384), so float math is fine. */
bool native_cursor_scale_rgba(const uint8_t *src, uint16_t src_w, uint16_t src_h, uint8_t *dst,
                              uint16_t dst_w, uint16_t dst_h) {
    if (!src || !dst || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return false;
    }
    const float step_x = (float)src_w / (float)dst_w;
    const float step_y = (float)src_h / (float)dst_h;
    uint8_t *dst_px = dst;
    for (uint32_t y = 0; y < dst_h; y++) {
        const float sy0 = (float)y * step_y;
        const float sy1 = sy0 + step_y;
        const uint32_t sy_first = (uint32_t)sy0;
        uint32_t sy_last = (uint32_t)(sy1 - 0.0001f);
        if (sy_last >= src_h) {
            sy_last = (uint32_t)(src_h - 1u);
        }
        for (uint32_t x = 0; x < dst_w; x++, dst_px += 4) {
            const float sx0 = (float)x * step_x;
            const float sx1 = sx0 + step_x;
            const uint32_t sx_first = (uint32_t)sx0;
            uint32_t sx_last = (uint32_t)(sx1 - 0.0001f);
            if (sx_last >= src_w) {
                sx_last = (uint32_t)(src_w - 1u);
            }
            float sum_w = 0.0f;
            float sum_a = 0.0f;
            float sum_r = 0.0f;
            float sum_g = 0.0f;
            float sum_b = 0.0f;
            for (uint32_t sy = sy_first; sy <= sy_last; sy++) {
                float wy = 1.0f;
                if ((float)sy < sy0) {
                    wy -= sy0 - (float)sy;
                }
                if ((float)sy + 1.0f > sy1) {
                    wy -= (float)sy + 1.0f - sy1;
                }
                const uint8_t *src_row = src + (size_t)sy * (size_t)src_w * 4u;
                for (uint32_t sx = sx_first; sx <= sx_last; sx++) {
                    float wx = 1.0f;
                    if ((float)sx < sx0) {
                        wx -= sx0 - (float)sx;
                    }
                    if ((float)sx + 1.0f > sx1) {
                        wx -= (float)sx + 1.0f - sx1;
                    }
                    const float w = wx * wy;
                    const uint8_t *px = src_row + (size_t)sx * 4u;
                    const float a = (float)px[3] * w;
                    sum_w += w;
                    sum_a += a;
                    sum_r += (float)px[0] * a;
                    sum_g += (float)px[1] * a;
                    sum_b += (float)px[2] * a;
                }
            }
            if (sum_a > 0.0f) {
                dst_px[0] = (uint8_t)(sum_r / sum_a + 0.5f);
                dst_px[1] = (uint8_t)(sum_g / sum_a + 0.5f);
                dst_px[2] = (uint8_t)(sum_b / sum_a + 0.5f);
                dst_px[3] = (uint8_t)(sum_a / (sum_w > 0.0f ? sum_w : 1.0f) + 0.5f);
            } else {
                memset(dst_px, 0, 4);
            }
        }
    }
    return true;
}

static uint16_t cursor_scale_dim(uint16_t value, uint16_t num, uint16_t den, uint16_t min) {
    uint32_t scaled = ((uint32_t)value * (uint32_t)num) / (uint32_t)den;
    if (scaled < min) {
        scaled = min;
    }
    if (scaled > UINT16_MAX) {
        scaled = UINT16_MAX;
    }
    return (uint16_t)scaled;
}

void native_cursor_scaled_geometry(uint16_t shape_w, uint16_t shape_h, uint16_t hot_x,
                                   uint16_t hot_y, uint16_t desktop_w, uint16_t desktop_h,
                                   uint16_t window_w, uint16_t window_h, uint16_t *out_w,
                                   uint16_t *out_h, uint16_t *out_hot_x, uint16_t *out_hot_y) {
    uint16_t w = shape_w;
    uint16_t h = shape_h;
    uint16_t hx = hot_x;
    uint16_t hy = hot_y;
    if (desktop_w != 0 && desktop_h != 0 && window_w != 0 && window_h != 0 &&
        (desktop_w != window_w || desktop_h != window_h)) {
        w = cursor_scale_dim(shape_w, window_w, desktop_w, 1);
        h = cursor_scale_dim(shape_h, window_h, desktop_h, 1);
        hx = cursor_scale_dim(hot_x, window_w, desktop_w, 0);
        hy = cursor_scale_dim(hot_y, window_h, desktop_h, 0);
    }
    /* Cap the mapped size: a hostile/degenerate desktop advertisement must not balloon
     * the shape into an allocation whose byte size overflows 32-bit size_t. Capped
     * hotspots are derived from the ORIGINAL shape geometry, not from the mapped
     * values: those may already be rounded — or saturated to UINT16_MAX by
     * cursor_scale_dim under extreme ratios, which would degenerate the hx/w
     * proportion to 1 and pin a centered anchor to the edge. The bitmap itself is
     * scaled straight from the source shape to the capped size, so this mapping is
     * the exact one. */
    if (w > NATIVE_CURSOR_MAX_SCALED_DIM) {
        w = NATIVE_CURSOR_MAX_SCALED_DIM;
        hx = (uint16_t)(((uint32_t)hot_x * w) / (shape_w ? shape_w : 1u));
    }
    if (h > NATIVE_CURSOR_MAX_SCALED_DIM) {
        h = NATIVE_CURSOR_MAX_SCALED_DIM;
        hy = (uint16_t)(((uint32_t)hot_y * h) / (shape_h ? shape_h : 1u));
    }
    if (hx >= w) {
        hx = (uint16_t)(w - 1u);
    }
    if (hy >= h) {
        hy = (uint16_t)(h - 1u);
    }
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    if (out_hot_x) {
        *out_hot_x = hx;
    }
    if (out_hot_y) {
        *out_hot_y = hy;
    }
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL

static void native_cursor_log_state(uint32_t state) {
    /* State transitions are sparse (unlike motion), and a late unexpected HIDDEN is the
     * key discriminator between an RDP-side hide and a lost webOS cursor plane. Do not
     * permanently suppress these after the first few minutes of a session. */
    clog(cLogLevelDebug, "applying server pointer %s",
         state == NATIVE_CURSOR_HIDDEN ? "hidden" : "default");
}

static bool native_cursor_platform_show(bool visible) {
#if HELLOLG_CURSOR_HAVE_WEBOS_VISIBILITY
    /* This webOS API can report SDL_FALSE with no SDL error on firmware where the
     * visibility request is still best-effort. Cursor shapes may be applied repeatedly
     * during motion, so logging every failure here stalls the hot path and can disturb
     * the shared media pipeline. Keep one diagnostic for each requested state. */
    static bool webos_failure_logged[2] = {false, false};
    /* Never SDL_ShowCursor(SDL_DISABLE) on webOS: it stops pointer-event delivery
     * entirely (verified live), so a server-driven hide (e.g. the remote browser hiding
     * the pointer over a video) would become unrecoverable by mouse input — our moves
     * would never reach the server and it would never re-show its cursor. The platform
     * visibility call hides only the image; events keep flowing and the system re-shows
     * the pointer on genuine activity, in step with the remote side doing the same. */
    SDL_ClearError();
    SDL_bool webos_result = SDL_webOSCursorVisibility(visible ? SDL_TRUE : SDL_FALSE);
    unsigned visibility_index = visible ? 1u : 0u;
    if (webos_result != SDL_TRUE && !webos_failure_logged[visibility_index]) {
        const char *error = SDL_GetError();
        webos_failure_logged[visibility_index] = true;
        clog(cLogLevelWarning, "SDL_webOSCursorVisibility(%d) failed: %s", visible ? 1 : 0,
             error && error[0] ? error : "no SDL error");
    }
    SDL_ClearError();
    int sdl_result = SDL_ShowCursor(SDL_ENABLE);
    if (sdl_result < 0) {
        clog_limited(cLogLevelWarning, 4, 5000, "SDL_ShowCursor(SDL_ENABLE) failed: %s",
                     SDL_GetError());
    }
    return webos_result == SDL_TRUE && sdl_result >= 0;
#else
    int result = SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
    if (result < 0) {
        clog_limited(cLogLevelWarning, 4, 5000, "SDL_ShowCursor(%d) failed: %s",
                     visible ? SDL_ENABLE : SDL_DISABLE, SDL_GetError());
    }
    return result >= 0;
#endif
}

static bool native_cursor_set_visible(NativeCursor *cursor, bool visible) {
    bool applied = native_cursor_platform_show(visible);
    if (applied) {
        cursor->visible = visible;
    }
    return applied;
}

void native_cursor_show_default(void) {
    SDL_Cursor *system_default = SDL_GetDefaultCursor();
    if (system_default) {
        SDL_SetCursor(system_default);
    }
    (void)native_cursor_platform_show(true);
}

void native_cursor_note_platform_visibility(NativeCursor *cursor, bool visible) {
    if (!cursor) {
        return;
    }
    pthread_mutex_lock(&cursor->lock);
    uint32_t desired = cursor->desired;
    uint32_t shape_serial = cursor->shape_serial;
    pthread_mutex_unlock(&cursor->lock);
    cursor->visible = visible;
    int sdl_visible = SDL_ShowCursor(SDL_QUERY);
    clog(cLogLevelInfo,
         "webOS reports platform pointer %s; server=%s shape_serial=%u built_serial=%u "
         "SDL_ShowCursor=%d",
         visible ? "shown" : "hidden",
         desired == NATIVE_CURSOR_HIDDEN
             ? "hidden"
             : (desired == NATIVE_CURSOR_SHAPE ? "shape" : "default"),
         (unsigned)shape_serial, (unsigned)cursor->built_serial, sdl_visible);
}

static void native_cursor_apply_shape(NativeCursor *cursor, uint8_t *rgba, uint16_t width,
                                      uint16_t height, uint16_t hot_x, uint16_t hot_y,
                                      uint16_t desktop_w, uint16_t desktop_h, uint16_t window_w,
                                      uint16_t window_h) {
    static unsigned log_count = 0;
    uint16_t dst_w = width;
    uint16_t dst_h = height;
    uint16_t dst_hx = hot_x;
    uint16_t dst_hy = hot_y;
    native_cursor_scaled_geometry(width, height, hot_x, hot_y, desktop_w, desktop_h, window_w,
                                  window_h, &dst_w, &dst_h, &dst_hx, &dst_hy);
    uint8_t *pixels = rgba;
    uint8_t *scaled = NULL;
    if (dst_w != width || dst_h != height) {
        scaled = (uint8_t *)malloc((size_t)dst_w * (size_t)dst_h * 4u);
        if (scaled && native_cursor_scale_rgba(rgba, width, height, scaled, dst_w, dst_h)) {
            pixels = scaled;
        } else {
            dst_w = width;
            dst_h = height;
            dst_hx = hot_x;
            dst_hy = hot_y;
        }
    }

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, dst_w, dst_h, 32,
                                                              (int)dst_w * 4, SDL_PIXELFORMAT_RGBA32);
    SDL_Cursor *sdl_cursor = surface ? SDL_CreateColorCursor(surface, dst_hx, dst_hy) : NULL;
    if (sdl_cursor) {
        SDL_SetCursor(sdl_cursor);
        if (cursor->cursor) {
            SDL_FreeCursor(cursor->cursor);
        }
        cursor->cursor = sdl_cursor;
        native_cursor_set_visible(cursor, true);
        if (log_count < 8) {
            if (pixels == scaled) {
                clog(cLogLevelDebug,
                     "server cursor %ux%u hotspot %u,%u (scaled to %ux%u)", (unsigned)width,
                     (unsigned)height, (unsigned)hot_x, (unsigned)hot_y, (unsigned)dst_w,
                     (unsigned)dst_h);
            } else {
                clog(cLogLevelDebug, "server cursor %ux%u hotspot %u,%u", (unsigned)width,
                     (unsigned)height, (unsigned)hot_x, (unsigned)hot_y);
            }
        } else if (log_count == 8) {
            clog(cLogLevelDebug, "further cursor shape logs suppressed");
        }
        log_count++;
    } else {
        /* Color cursors are unproven on the webOS SDL port; degrade to the visible default
         * arrow (the server draws no pointer of its own and considers it shown, so leaving
         * it hidden would strand the user with no pointer). Probe line logged once. */
        if (!cursor->color_cursor_unavailable) {
            /* The fallback visibility calls clear SDL's error state for their own
             * diagnostics, so report the cursor-creation failure before invoking them. */
            cursor->color_cursor_unavailable = true;
            clog(cLogLevelWarning, "color cursor unavailable: %s", SDL_GetError());
        }
        SDL_Cursor *system_default = SDL_GetDefaultCursor();
        if (system_default) {
            SDL_SetCursor(system_default);
        }
        if (cursor->cursor) {
            SDL_FreeCursor(cursor->cursor);
            cursor->cursor = NULL;
        }
        native_cursor_set_visible(cursor, true);
    }
    if (surface) {
        SDL_FreeSurface(surface);
    }
    free(scaled);
}

void native_cursor_apply(NativeCursor *cursor, uint16_t desktop_w, uint16_t desktop_h,
                         uint16_t window_w, uint16_t window_h) {
    if (!cursor) {
        return;
    }
    unsigned generation = atomic_load(&cursor->generation);
    /* A changed desktop-to-window mapping (RESET_GRAPHICS resize; the window is fixed
     * on the TV) must rebuild the cursor even with no new pointer update: the shape's
     * scale and hotspot are functions of that mapping. */
    bool geometry_changed = desktop_w != cursor->applied_desktop_w || desktop_h != cursor->applied_desktop_h ||
                            window_w != cursor->applied_window_w || window_h != cursor->applied_window_h;
    if (generation == cursor->applied_generation && !geometry_changed) {
        return;
    }
    pthread_mutex_lock(&cursor->lock);
    uint32_t desired = cursor->desired;
    uint32_t shape_serial = cursor->shape_serial;
    uint16_t width = cursor->shape_width;
    uint16_t height = cursor->shape_height;
    uint16_t hot_x = cursor->hotspot_x;
    uint16_t hot_y = cursor->hotspot_y;
    uint8_t *rgba = NULL;
    if (desired == NATIVE_CURSOR_SHAPE && cursor->shape_rgba &&
        (shape_serial != cursor->built_serial || geometry_changed || !cursor->cursor)) {
        /* Rebuild path: work on a COPY so the retained original can serve the next
         * geometry change too, and so the SDL work below runs outside the lock. */
        size_t len = (size_t)width * (size_t)height * 4u;
        rgba = (uint8_t *)malloc(len);
        if (rgba) {
            memcpy(rgba, cursor->shape_rgba, len);
        }
    }
    pthread_mutex_unlock(&cursor->lock);
    cursor->applied_generation = generation;
    cursor->applied_desktop_w = desktop_w;
    cursor->applied_desktop_h = desktop_h;
    cursor->applied_window_w = window_w;
    cursor->applied_window_h = window_h;

    /* Each case asserts the platform state unconditionally rather than short-circuiting on
     * cursor->visible: that flag can be stale (the webOS compositor auto-hides/shows the
     * pointer on idle/activity behind our back), so a guard there would skip a genuinely-needed
     * re-hide or re-show. The SDL calls are idempotent and only run when the server actually
     * changed the pointer. */
    switch (desired) {
    case NATIVE_CURSOR_SHAPE:
        if (rgba) {
            native_cursor_apply_shape(cursor, rgba, width, height, hot_x, hot_y, desktop_w,
                                      desktop_h, window_w, window_h);
            cursor->built_serial = shape_serial;
        } else if (cursor->cursor) {
            /* Same artwork at the same geometry (e.g. hidden -> shape again): the built
             * SDL cursor is still correct, just re-assert it. */
            SDL_SetCursor(cursor->cursor);
            native_cursor_set_visible(cursor, true);
        } else {
            /* No shape retained (or the copy failed): visible beats stranded. */
            native_cursor_set_visible(cursor, true);
        }
        break;
    case NATIVE_CURSOR_HIDDEN:
        if (cursor->visible) {
            native_cursor_log_state(NATIVE_CURSOR_HIDDEN);
        }
        native_cursor_set_visible(cursor, false);
        break;
    case NATIVE_CURSOR_DEFAULT:
    default: {
        SDL_Cursor *system_default = SDL_GetDefaultCursor();
        if (system_default) {
            SDL_SetCursor(system_default);
        }
        if (cursor->cursor) {
            SDL_FreeCursor(cursor->cursor);
            cursor->cursor = NULL;
        }
        if (!cursor->visible) {
            native_cursor_log_state(NATIVE_CURSOR_DEFAULT);
        }
        native_cursor_set_visible(cursor, true);
        break;
    }
    }
    free(rgba);
}

void native_cursor_reset(NativeCursor *cursor) {
    if (!cursor) {
        return;
    }
    pthread_mutex_lock(&cursor->lock);
    free(cursor->shape_rgba);
    cursor->shape_rgba = NULL;
    cursor->desired = NATIVE_CURSOR_DEFAULT;
    pthread_mutex_unlock(&cursor->lock);
    cursor->applied_generation = atomic_load(&cursor->generation);
    SDL_Cursor *system_default = SDL_GetDefaultCursor();
    if (system_default) {
        SDL_SetCursor(system_default);
    }
    if (cursor->cursor) {
        SDL_FreeCursor(cursor->cursor);
        cursor->cursor = NULL;
    }
    native_cursor_set_visible(cursor, true);
}

void native_cursor_reassert(NativeCursor *cursor) {
    if (!cursor) {
        return;
    }
    /* Re-apply the last server-driven pointer state after a webOS overlay (TV menu) hid the
     * platform pointer behind our back: unlike native_cursor_apply this ignores the
     * generation gate, and unlike the old typing-hide recovery it does not skip when we think
     * the cursor is already visible (the platform state is out of sync after the overlay). A
     * server-requested hide is still honoured. The worker writes desired under the lock, so
     * snapshot it under the same lock (focus regain can race an RDP pointer update). */
    pthread_mutex_lock(&cursor->lock);
    uint32_t desired = cursor->desired;
    pthread_mutex_unlock(&cursor->lock);
    if (desired == NATIVE_CURSOR_HIDDEN) {
        (void)native_cursor_set_visible(cursor, false);
        return;
    }
    /* Install the artwork first, then ask webOS to expose the cursor plane. Every other
     * show path uses this ordering; doing it backwards here allowed a cursor replacement
     * to race/undo the visibility request on some firmware revisions. */
    if (cursor->cursor) {
        SDL_SetCursor(cursor->cursor);
    } else {
        SDL_Cursor *system_default = SDL_GetDefaultCursor();
        if (system_default) {
            SDL_SetCursor(system_default);
        }
    }
    (void)native_cursor_set_visible(cursor, true);
}

#endif /* HELLOLG_WITH_SDL */
