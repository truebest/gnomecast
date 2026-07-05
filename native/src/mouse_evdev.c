#include "mouse_evdev.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "evmouse.h"
#include "logging.h"

/* evmouse.c (vendored from third_party/commons) logs through commons_log_printf; provide a
 * minimal implementation to stderr so we can link it without pulling in commons-logging. */
void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[evmouse:%s] ", tag ? tag : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void mouse_evdev_push(NativeMouseEvdev *m, const NativeMouseEv *ev) {
    pthread_mutex_lock(&m->lock);
    /* Coalesce consecutive motion so a fast pointer can't overflow the ring; buttons and
     * wheel always push a new slot so their order relative to motion (drag semantics) is
     * preserved. */
    if (ev->kind == NATIVE_MOUSE_EV_MOTION && m->head != m->tail) {
        unsigned last = (m->tail + NATIVE_MOUSE_EV_RING - 1u) % NATIVE_MOUSE_EV_RING;
        if (m->ring[last].kind == NATIVE_MOUSE_EV_MOTION) {
            m->ring[last].dx += ev->dx;
            m->ring[last].dy += ev->dy;
            pthread_mutex_unlock(&m->lock);
            return;
        }
    }
    unsigned next = (m->tail + 1u) % NATIVE_MOUSE_EV_RING;
    if (next == m->head) {
        /* Ring full: drop the oldest to keep the newest (input latency beats a stall). */
        m->head = (m->head + 1u) % NATIVE_MOUSE_EV_RING;
    }
    m->ring[m->tail] = *ev;
    m->tail = next;
    pthread_mutex_unlock(&m->lock);
}

static void mouse_evdev_listener(const evmouse_event_t *event, void *userdata) {
    NativeMouseEvdev *m = (NativeMouseEvdev *)userdata;
    atomic_fetch_add(&m->event_count, 1u);
    NativeMouseEv ev;
    memset(&ev, 0, sizeof(ev));
    switch (event->type) {
    case SDL_MOUSEMOTION:
        ev.kind = NATIVE_MOUSE_EV_MOTION;
        ev.dx = event->motion.xrel;
        ev.dy = event->motion.yrel;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        ev.kind = NATIVE_MOUSE_EV_BUTTON;
        ev.sdl_button = event->button.button;
        ev.down = event->type == SDL_MOUSEBUTTONDOWN;
        break;
    case SDL_MOUSEWHEEL:
        ev.kind = NATIVE_MOUSE_EV_WHEEL;
        ev.wheel_x = event->wheel.x;
        ev.wheel_y = event->wheel.y;
        break;
    default:
        return;
    }
    mouse_evdev_push(m, &ev);
}

static void *mouse_evdev_thread(void *arg) {
    NativeMouseEvdev *m = (NativeMouseEvdev *)arg;
    evmouse_listen((evmouse_t *)m->dev, mouse_evdev_listener, m);
    return NULL;
}

bool native_mouse_evdev_start(NativeMouseEvdev *m) {
    if (!m || m->started) {
        return m && m->started;
    }
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->lock, NULL);
    atomic_init(&m->running, false);
    atomic_init(&m->event_count, 0u);

    evmouse_t *dev = evmouse_open_default();
    if (!dev) {
        fprintf(stderr, "[native-mouse] no USB mouse device; using the compositor pointer (Magic Remote) via SDL\n");
        pthread_mutex_destroy(&m->lock);
        return false;
    }
    /* Grab the device so the compositor stops processing the mouse (no synthesized Back on
     * right click, no recenter warp). Best-effort: on failure evmouse logs and continues
     * ungrabbed, but the reader still runs, so native_mouse_evdev_active() stays true and the
     * SDL mouse fallback stays gated off (no double-processing). Grab success is validated
     * on-device: SDL motion should go silent while a USB mouse moves. */
    evmouse_set_grab(dev, SDL_TRUE);
    m->dev = dev;
    atomic_store(&m->running, true);
    if (pthread_create(&m->thread, NULL, mouse_evdev_thread, m) != 0) {
        fprintf(stderr, "[native-mouse] failed to start evdev reader thread; using the SDL pointer path\n");
        evmouse_close(dev);
        m->dev = NULL;
        atomic_store(&m->running, false);
        pthread_mutex_destroy(&m->lock);
        return false;
    }
    m->started = true;
    fprintf(stderr, "[native-mouse] evdev mouse reader started (grabbed)\n");
    return true;
}

void native_mouse_evdev_stop(NativeMouseEvdev *m) {
    if (!m || !m->started) {
        return;
    }
    atomic_store(&m->running, false);
    if (m->dev) {
        evmouse_interrupt((evmouse_t *)m->dev);
    }
    pthread_join(m->thread, NULL);
    if (m->dev) {
        evmouse_close((evmouse_t *)m->dev);
        m->dev = NULL;
    }
    pthread_mutex_destroy(&m->lock);
    m->started = false;
}

bool native_mouse_evdev_active(const NativeMouseEvdev *m) {
    return m && m->started && atomic_load(&m->running);
}

bool native_mouse_evdev_pop(NativeMouseEvdev *m, NativeMouseEv *out) {
    if (!m || !m->started || !out) {
        return false;
    }
    bool got = false;
    pthread_mutex_lock(&m->lock);
    if (m->head != m->tail) {
        *out = m->ring[m->head];
        m->head = (m->head + 1u) % NATIVE_MOUSE_EV_RING;
        got = true;
    }
    pthread_mutex_unlock(&m->lock);
    return got;
}
