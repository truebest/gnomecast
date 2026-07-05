#ifndef GNOMECAST_KEYBOARD_EVDEV_H
#define GNOMECAST_KEYBOARD_EVDEV_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Raw evdev keyboard reader. On webOS the compositor consumes many keys before the app
 * (F1-F12 map to TV functions, keypad-5 without NumLock, right-click's synthetic Back) and
 * only delivers a munged SDL stream. For a full RDP desktop we want every key. Reading the
 * USB keyboard straight from /dev/input, below the compositor, gives raw Linux keycodes we
 * forward as RDP scancodes — no IME/unicode, no Back synthesis, no NumLock quirk.
 *
 * Only the attached USB keyboard is grabbed (EVIOCGRAB), never the TV remote, so the remote's
 * Home/Back/Exit still work and the user can always background the app (which releases the
 * grab). The grab is global, so its lifecycle is tied to app focus exactly like the mouse. */

typedef struct NativeKeyboardEv {
    uint16_t code; /* Linux input event code (KEY_*) */
    bool down;     /* press (also autorepeat) vs release */
} NativeKeyboardEv;

#define NATIVE_KEYBOARD_EV_RING 256u

typedef struct NativeKeyboardEvdev {
    pthread_mutex_t lock;
    NativeKeyboardEv ring[NATIVE_KEYBOARD_EV_RING];
    unsigned head;
    unsigned tail;
    pthread_t thread;
    int fds[8];    /* grabbed keyboard fds */
    int nfds;
    int wake_pipe[2]; /* self-pipe to interrupt the blocking select() */
    atomic_bool running;
    bool started;
} NativeKeyboardEvdev;

/* Scan /dev/input for a real typing keyboard without opening a reader or grabbing anything.
 * Cheap presence check used before connecting to warn when no USB keyboard is attached (there
 * is no SDL keyboard fallback). May race a hotplug, so it is a hint, not a guarantee. */
bool native_keyboard_evdev_probe(void);

/* Open the USB keyboard device(s), grab (best-effort), and spawn the reader thread.
 * Returns true if a keyboard was found and the reader started; false otherwise. */
bool native_keyboard_evdev_start(NativeKeyboardEvdev *k);

/* Interrupt the reader, join, ungrab and close. */
void native_keyboard_evdev_stop(NativeKeyboardEvdev *k);

/* True while the reader thread is active (SDL real-key handling should be ignored then). */
bool native_keyboard_evdev_active(const NativeKeyboardEvdev *k);

/* SDL thread: pop the next queued key event. Returns false when the ring is empty. */
bool native_keyboard_evdev_pop(NativeKeyboardEvdev *k, NativeKeyboardEv *out);

#endif
