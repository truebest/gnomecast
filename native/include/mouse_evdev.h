#ifndef GNOMECAST_MOUSE_EVDEV_H
#define GNOMECAST_MOUSE_EVDEV_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Raw evdev mouse reader. On webOS the SDL/Wayland pointer layer munges physical mouse
 * input for TV use (it synthesizes a Back key on right click, recenters the pointer around
 * right clicks / IME, and auto-hides the cursor). Reading the mouse straight from
 * /dev/input, below the compositor, avoids all of that. A background pthread runs the
 * blocking evmouse read loop and posts events into an ordered ring the SDL thread drains
 * each tick. Motion is relative; the SDL thread integrates it into the logical pointer and
 * warps the OS pointer so the server cursor (drawn on the platform cursor plane) follows.
 *
 * Only the mouse is read this way; the keyboard stays on SDL. Grabbing (EVIOCGRAB) makes
 * the compositor stop processing the mouse (no Back, no recenter); it is best-effort. */

typedef enum NativeMouseEvKind {
    NATIVE_MOUSE_EV_MOTION = 0, /* relative dx/dy */
    NATIVE_MOUSE_EV_BUTTON = 1, /* button down/up */
    NATIVE_MOUSE_EV_WHEEL = 2,  /* wheel delta */
} NativeMouseEvKind;

typedef struct NativeMouseEv {
    NativeMouseEvKind kind;
    int dx, dy;          /* MOTION: accumulated relative delta */
    uint8_t sdl_button;  /* BUTTON: SDL_BUTTON_* value */
    bool down;           /* BUTTON: press vs release */
    int wheel_x, wheel_y; /* WHEEL: axis deltas */
} NativeMouseEv;

#define NATIVE_MOUSE_EV_RING 512u

typedef struct NativeMouseEvdev {
    pthread_mutex_t lock;
    NativeMouseEv ring[NATIVE_MOUSE_EV_RING];
    unsigned head; /* pop index */
    unsigned tail; /* push index */
    pthread_t thread;
    void *dev; /* evmouse_t* */
    atomic_bool running;
    bool started;
    /* Diagnostics (reader thread writes, drain reads — advisory only). */
    atomic_uint event_count;
} NativeMouseEvdev;

/* Open the default mouse device, grab it (best-effort), and spawn the reader thread.
 * Returns true if a mouse was found and the reader started; false otherwise (caller then
 * keeps the SDL mouse path). Safe to call once; idempotent-guarded by `started`. */
bool native_mouse_evdev_start(NativeMouseEvdev *m);

/* Interrupt the reader loop, join the thread, ungrab and close the device. */
void native_mouse_evdev_stop(NativeMouseEvdev *m);

/* True while the reader thread is active (the SDL mouse path should be ignored then). */
bool native_mouse_evdev_active(const NativeMouseEvdev *m);

/* SDL thread: pop the next queued event into *out. Returns false when the ring is empty. */
bool native_mouse_evdev_pop(NativeMouseEvdev *m, NativeMouseEv *out);

#endif
