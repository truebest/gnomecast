#ifndef GNOMECAST_CURSOR_SDL_H
#define GNOMECAST_CURSOR_SDL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
#include <SDL.h>
#endif

/* Server-driven mouse cursor. Shapes and visibility arrive as RDP pointer updates on the
 * RDP worker thread (submit_*); the SDL thread applies them once per loop tick via
 * native_cursor_apply, turning the latest shape into a system color cursor
 * (SDL_CreateColorCursor). Rendering through the platform cursor plane keeps the
 * "one punch frame, then never present" contract of the hardware video path intact. */

/* Desired presentation, extends RDP_POINTER_STATE_* (rdp_ffi.h). */
enum {
    NATIVE_CURSOR_HIDDEN = 0,  /* == RDP_POINTER_STATE_HIDDEN */
    NATIVE_CURSOR_DEFAULT = 1, /* == RDP_POINTER_STATE_DEFAULT */
    NATIVE_CURSOR_SHAPE = 2,   /* show the latest server-provided shape */
};

#define NATIVE_CURSOR_MAX_DIM 384u /* MS-RDPBCGR large pointer limit */
/* Ceiling on the WINDOW-mapped cursor size: a server advertising a tiny desktop would
 * otherwise scale a legal 384px shape toward 65535px, whose w*h*4 allocation size
 * overflows 32-bit size_t on the armv7 target (a small malloc followed by full-size
 * writes = heap corruption). 1024px is far beyond any sane pointer. */
#define NATIVE_CURSOR_MAX_SCALED_DIM 1024u

typedef struct NativeCursor {
    pthread_mutex_t lock;
    /* Pending state (written on the RDP worker thread, consumed on the SDL thread). */
    uint32_t desired;
    /* malloc'd width*height*4. RETAINED across applies (replaced by the next submit,
     * freed by reset/destroy): a desktop-resolution change (RESET_GRAPHICS) must be
     * able to rebuild the SDL cursor at the new scale without waiting for the server
     * to happen to send another shape. */
    uint8_t *shape_rgba;
    uint16_t shape_width;
    uint16_t shape_height;
    uint16_t hotspot_x;
    uint16_t hotspot_y;
    /* Bumped with every new shape bitmap (guarded by lock): tells the SDL thread "new
     * artwork" apart from "same artwork, new geometry". */
    uint32_t shape_serial;
    /* Bumped on every submit; lets the SDL thread skip the mutex when idle. */
    atomic_uint generation;
    unsigned applied_generation;
#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
    SDL_Cursor *cursor; /* SDL thread only */
    /* SDL thread only: what the current SDL cursor was built from/for, so apply can
     * rebuild when the desktop-to-window mapping changes under an unchanged shape. */
    uint32_t built_serial;
    uint16_t applied_desktop_w, applied_desktop_h;
    uint16_t applied_window_w, applied_window_h;
    bool color_cursor_unavailable;
    bool visible;
#endif
} NativeCursor;

void native_cursor_init(NativeCursor *cursor);
void native_cursor_destroy(NativeCursor *cursor);

/* RDP worker thread side. submit_bitmap validates len == width*height*4 and the
 * NATIVE_CURSOR_MAX_DIM bound; invalid shapes are dropped. */
void native_cursor_submit_bitmap(NativeCursor *cursor, uint16_t width, uint16_t height,
                                 uint16_t hotspot_x, uint16_t hotspot_y, const uint8_t *rgba,
                                 size_t len);
void native_cursor_submit_state(NativeCursor *cursor, uint32_t state);

/* Pure helpers (no SDL, unit-tested). */

/* Nearest-neighbour scale of a tight-stride RGBA image. dst must hold dst_w*dst_h*4. */
bool native_cursor_scale_rgba(const uint8_t *src, uint16_t src_w, uint16_t src_h, uint8_t *dst,
                              uint16_t dst_w, uint16_t dst_h);

/* Size/hotspot of a server shape mapped onto the window canvas. The shape is sized for the
 * server desktop (e.g. 4K) while the cursor plane lives on the window canvas (e.g. 1080p);
 * unknown/degenerate sizes fall back to 1:1. Outputs are clamped to >= 1 (hotspot >= 0). */
void native_cursor_scaled_geometry(uint16_t shape_w, uint16_t shape_h, uint16_t hot_x,
                                   uint16_t hot_y, uint16_t desktop_w, uint16_t desktop_h,
                                   uint16_t window_w, uint16_t window_h, uint16_t *out_w,
                                   uint16_t *out_h, uint16_t *out_hot_x, uint16_t *out_hot_y);

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
/* SDL thread: apply pending shape/state; cheap no-op while generation is unchanged. */
void native_cursor_apply(NativeCursor *cursor, uint16_t desktop_w, uint16_t desktop_h,
                         uint16_t window_w, uint16_t window_h);
/* SDL thread: drop server state and restore the default visible arrow (session ended). */
void native_cursor_reset(NativeCursor *cursor);
/* SDL thread: show the platform default arrow for a UI screen WITHOUT touching any
 * session's cached pointer state — for leaving a stream whose session keeps running
 * backgrounded (a later switch-back reasserts its cached cursor; the server does not
 * resend shapes while suppressed). */
void native_cursor_show_default(void);
/* SDL thread: record a cursor show/hide pseudo-event emitted by webOS itself. This keeps
 * the diagnostic/tracked platform state separate from the server's desired state; the
 * caller may then reassert the server state on real pointer activity. */
void native_cursor_note_platform_visibility(NativeCursor *cursor, bool visible);
/* SDL thread: force the current server pointer state back onto the platform after a webOS
 * overlay (TV menu) hid the pointer behind our back — e.g. on window focus regain. Honours a
 * server-requested hide; otherwise re-shows and re-sets the last cursor shape. */
void native_cursor_reassert(NativeCursor *cursor);
#endif

#endif
