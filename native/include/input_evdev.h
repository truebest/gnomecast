#ifndef GNOMECAST_INPUT_EVDEV_H
#define GNOMECAST_INPUT_EVDEV_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Raw /dev/input reader for grabbed USB mouse and keyboard devices.
 *
 * One background thread polls all selected evdev fds, translates kernel input_event records
 * into compact mouse/key rings, and wakes the SDL thread through eventfd so input is not
 * gated by the render-loop sleep. The SDL thread drains batches and forwards them to RDP. */

typedef enum NativeMouseEvKind {
    NATIVE_MOUSE_EV_MOTION = 0, /* relative dx/dy */
    NATIVE_MOUSE_EV_BUTTON = 1, /* button down/up */
    NATIVE_MOUSE_EV_WHEEL = 2,  /* wheel delta */
} NativeMouseEvKind;

typedef struct NativeMouseEv {
    NativeMouseEvKind kind;
    int dx, dy;           /* MOTION: accumulated relative delta */
    uint8_t sdl_button;   /* BUTTON: SDL_BUTTON_* value */
    bool down;            /* BUTTON: press vs release */
    int wheel_x, wheel_y; /* WHEEL: axis deltas */
} NativeMouseEv;

typedef struct NativeKeyboardEv {
    uint16_t code; /* Linux input event code (KEY_*) */
    bool down;     /* press (also autorepeat) vs release */
    /* True only for a TV-remote virtual node (RCU / LGE Network Input). This
     * lets app-level remote shortcuts coexist with the same keys on a physical
     * USB keyboard, which must keep flowing to RDP. */
    bool from_remote;
} NativeKeyboardEv;

#define NATIVE_EVDEV_MOUSE_RING 512u
#define NATIVE_EVDEV_KEYBOARD_RING 256u

typedef struct NativeEvdevInput {
    pthread_mutex_t lock;
    NativeMouseEv mouse_ring[NATIVE_EVDEV_MOUSE_RING];
    unsigned mouse_head;
    unsigned mouse_tail;
    NativeKeyboardEv keyboard_ring[NATIVE_EVDEV_KEYBOARD_RING];
    unsigned keyboard_head;
    unsigned keyboard_tail;
    pthread_t thread;
    void *backend;
    int wake_fd; /* eventfd: reader wakes SDL/main loop after queued input */
    int stop_fd; /* eventfd: stop wakes the poll()ing reader */
    atomic_bool running;
    bool lock_initialized;
    bool started;
    atomic_bool mouse_active;
    atomic_bool keyboard_active;
    atomic_uint event_count; /* advisory diagnostics */
} NativeEvdevInput;

bool native_evdev_input_probe_keyboard(void);
bool native_evdev_input_probe_mouse(void);
bool native_evdev_input_start(NativeEvdevInput *input);
void native_evdev_input_stop(NativeEvdevInput *input);
bool native_evdev_input_active(const NativeEvdevInput *input);
bool native_evdev_input_mouse_active(NativeEvdevInput *input);
bool native_evdev_input_keyboard_active(NativeEvdevInput *input);

int native_evdev_input_wake_fd(const NativeEvdevInput *input);
void native_evdev_input_clear_wake(NativeEvdevInput *input);

size_t native_evdev_input_pop_mouse_batch(NativeEvdevInput *input, NativeMouseEv *out, size_t cap);
size_t native_evdev_input_pop_keyboard_batch(NativeEvdevInput *input, NativeKeyboardEv *out, size_t cap);

#endif
