#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "input_evdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <SDL.h>

#define NATIVE_EVDEV_MAX_DEVICES 16
/* Consecutive poll() failures tolerated before the reader gives up (see evdev_thread). */
#define NATIVE_EVDEV_MAX_POLL_FAILURES 100

typedef struct NativeEvdevDevice {
    int fd;
    struct libevdev *dev;
    /* A single /dev/input node can be both a pointer and a keyboard (e.g. an all-in-one
     * wireless keyboard+trackpad on one dongle), so these are independent capabilities rather
     * than one exclusive kind; events are routed per event type in evdev_dispatch. */
    bool is_mouse;
    bool is_keyboard;
    char path[64]; /* /dev/input node path, used to de-duplicate hotplug re-scans */
} NativeEvdevDevice;

typedef struct NativeEvdevBackend {
    NativeEvdevDevice devices[NATIVE_EVDEV_MAX_DEVICES];
    int ndevices;
    int inotify_fd; /* watches /dev/input for hotplugged nodes; -1 when unavailable */
} NativeEvdevBackend;

typedef struct NativeEvdevDrainResult {
    bool pushed;
    bool remove;
} NativeEvdevDrainResult;

static bool evdev_is_mouse(const struct libevdev *dev) {
    return libevdev_has_event_code(dev, EV_KEY, BTN_MOUSE) && libevdev_has_event_code(dev, EV_REL, REL_X) &&
           libevdev_has_event_code(dev, EV_REL, REL_Y);
}

/* True for a device we should grab as a keyboard: it has the typing keys AND is either a pure
 * keyboard (no pointer buttons) or a genuine combined keyboard+trackpad (also a real relative
 * mouse). A node that carries pointer buttons but is NOT a relative mouse is a TV remote / gyro
 * pointer that merely advertises a full keymap (on webOS: "LGE RCU", "M-RCU", "CHECK INPUT",
 * "IoT keypad", "Bluetooth-audio-source", ...). Grabbing one of those steals the remote from
 * the compositor, so it must be rejected here. */
static bool evdev_is_keyboard(const struct libevdev *dev) {
    if (!(libevdev_has_event_code(dev, EV_KEY, KEY_A) && libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
          libevdev_has_event_code(dev, EV_KEY, KEY_SPACE))) {
        return false;
    }
    bool has_pointer_buttons =
        libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) || libevdev_has_event_code(dev, EV_KEY, BTN_MOUSE);
    return !has_pointer_buttons || evdev_is_mouse(dev);
}

/* Visit each /dev/input/event* node, calling `visit(path, user)`; stops as soon as a visit
 * returns true and reports whether that happened. Shared by keyboard probing and grab setup so
 * the two never disagree about which nodes exist or how they are enumerated. */
typedef bool (*NativeEvdevNodeVisitor)(const char *path, void *user);

static bool evdev_for_each_input_node(NativeEvdevNodeVisitor visit, void *user) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return false;
    }
    bool stopped = false;
    struct dirent *ent;
    while (!stopped && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[300];
        (void)snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        stopped = visit(path, user);
    }
    closedir(dir);
    return stopped;
}

static bool evdev_probe_keyboard_node(const char *path, void *user) {
    (void)user;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct libevdev *dev = NULL;
    bool is_keyboard = false;
    if (libevdev_new_from_fd(fd, &dev) == 0) {
        is_keyboard = evdev_is_keyboard(dev);
        libevdev_free(dev);
    }
    close(fd);
    return is_keyboard; /* stop at the first keyboard found */
}

bool native_evdev_input_probe_keyboard(void) {
    return evdev_for_each_input_node(evdev_probe_keyboard_node, NULL);
}

static uint8_t evdev_mouse_button(uint16_t code) {
    switch (code) {
    case BTN_LEFT:
        return SDL_BUTTON_LEFT;
    case BTN_RIGHT:
        return SDL_BUTTON_RIGHT;
    case BTN_MIDDLE:
        return SDL_BUTTON_MIDDLE;
    case BTN_SIDE:
        return SDL_BUTTON_X1;
    case BTN_EXTRA:
        return SDL_BUTTON_X2;
    case BTN_FORWARD:
        return SDL_BUTTON_X2 + 1;
    case BTN_BACK:
        return SDL_BUTTON_X2 + 2;
    case BTN_TASK:
        return SDL_BUTTON_X2 + 3;
    default:
        return 0;
    }
}

static bool evdev_push_mouse(NativeEvdevInput *input, const NativeMouseEv *ev) {
    pthread_mutex_lock(&input->lock);
    if (ev->kind == NATIVE_MOUSE_EV_MOTION && input->mouse_head != input->mouse_tail) {
        unsigned last = (input->mouse_tail + NATIVE_EVDEV_MOUSE_RING - 1u) % NATIVE_EVDEV_MOUSE_RING;
        if (input->mouse_ring[last].kind == NATIVE_MOUSE_EV_MOTION) {
            input->mouse_ring[last].dx += ev->dx;
            input->mouse_ring[last].dy += ev->dy;
            pthread_mutex_unlock(&input->lock);
            return true;
        }
    }
    unsigned next = (input->mouse_tail + 1u) % NATIVE_EVDEV_MOUSE_RING;
    if (next == input->mouse_head) {
        input->mouse_head = (input->mouse_head + 1u) % NATIVE_EVDEV_MOUSE_RING;
    }
    input->mouse_ring[input->mouse_tail] = *ev;
    input->mouse_tail = next;
    pthread_mutex_unlock(&input->lock);
    return true;
}

static bool evdev_push_keyboard(NativeEvdevInput *input, uint16_t code, bool down) {
    pthread_mutex_lock(&input->lock);
    unsigned next = (input->keyboard_tail + 1u) % NATIVE_EVDEV_KEYBOARD_RING;
    if (next == input->keyboard_head) {
        input->keyboard_head = (input->keyboard_head + 1u) % NATIVE_EVDEV_KEYBOARD_RING;
    }
    input->keyboard_ring[input->keyboard_tail].code = code;
    input->keyboard_ring[input->keyboard_tail].down = down;
    input->keyboard_tail = next;
    pthread_mutex_unlock(&input->lock);
    return true;
}

static bool evdev_dispatch_mouse(NativeEvdevInput *input, const struct input_event *raw) {
    NativeMouseEv ev;
    memset(&ev, 0, sizeof(ev));
    switch (raw->type) {
    case EV_REL:
        switch (raw->code) {
        case REL_X:
            ev.kind = NATIVE_MOUSE_EV_MOTION;
            ev.dx = raw->value;
            break;
        case REL_Y:
            ev.kind = NATIVE_MOUSE_EV_MOTION;
            ev.dy = raw->value;
            break;
        case REL_HWHEEL:
            ev.kind = NATIVE_MOUSE_EV_WHEEL;
            ev.wheel_x = raw->value;
            break;
        case REL_WHEEL:
            ev.kind = NATIVE_MOUSE_EV_WHEEL;
            ev.wheel_y = raw->value;
            break;
        default:
            return false;
        }
        break;
    case EV_KEY:
        if (raw->code < BTN_LEFT || raw->code > BTN_TASK) {
            return false;
        }
        ev.kind = NATIVE_MOUSE_EV_BUTTON;
        ev.sdl_button = evdev_mouse_button((uint16_t)raw->code);
        if (ev.sdl_button == 0) {
            return false;
        }
        ev.down = raw->value != 0;
        break;
    default:
        return false;
    }
    atomic_fetch_add(&input->event_count, 1u);
    return evdev_push_mouse(input, &ev);
}

static bool evdev_dispatch_keyboard(NativeEvdevInput *input, const struct input_event *raw) {
    if (raw->type != EV_KEY) {
        return false;
    }
    atomic_fetch_add(&input->event_count, 1u);
    return evdev_push_keyboard(input, (uint16_t)raw->code, raw->value != 0);
}

static bool evdev_dispatch(NativeEvdevInput *input, const NativeEvdevDevice *device, const struct input_event *raw) {
    switch (raw->type) {
    case EV_REL:
        /* Relative motion/wheel is always pointer data. */
        return device->is_mouse ? evdev_dispatch_mouse(input, raw) : false;
    case EV_KEY:
        /* Pointer buttons (BTN_LEFT..BTN_TASK) belong to the mouse ring; every other EV_KEY is
         * a typing key for the keyboard ring. A combined device routes each to its ring. */
        if (raw->code >= BTN_LEFT && raw->code <= BTN_TASK) {
            return device->is_mouse ? evdev_dispatch_mouse(input, raw) : false;
        }
        /* Remote color keys and the channel rocker (session switching/zapping) ride
         * whatever grabbed node emits them: some Magic Remote firmwares expose virtual
         * nodes that qualify only as relative mice, and the switch keys must not depend
         * on that node also typing like a keyboard. The keyboard drain runs whenever the
         * ring holds events, so these get through even with no keyboard-classified
         * device present. (KEY_RED..KEY_BLUE and KEY_CHANNELUP/DOWN are contiguous.) */
        if (raw->code >= KEY_RED && raw->code <= KEY_CHANNELDOWN) {
            return evdev_dispatch_keyboard(input, raw);
        }
        return device->is_keyboard ? evdev_dispatch_keyboard(input, raw) : false;
    default:
        return false;
    }
}

static void evdev_notify_main(NativeEvdevInput *input) {
    if (!input || input->wake_fd < 0) {
        return;
    }
    uint64_t one = 1;
    if (write(input->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        fprintf(stderr, "[native-input] failed to signal input eventfd: %s\n", strerror(errno));
    }
}

static NativeEvdevDrainResult evdev_drain_sync(NativeEvdevInput *input, NativeEvdevDevice *device) {
    NativeEvdevDrainResult result = {0};
    for (;;) {
        struct input_event raw;
        int ret = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_SYNC, &raw);
        if (ret == LIBEVDEV_READ_STATUS_SYNC) {
            result.pushed = evdev_dispatch(input, device, &raw) || result.pushed;
        } else if (ret == -EAGAIN) {
            break;
        } else {
            fprintf(stderr, "[native-input] libevdev sync failed on fd %d: %s\n", device->fd, strerror(-ret));
            result.remove = true;
            break;
        }
    }
    return result;
}

static NativeEvdevDrainResult evdev_drain_device(NativeEvdevInput *input, NativeEvdevDevice *device) {
    NativeEvdevDrainResult result = {0};
    for (;;) {
        struct input_event raw;
        int ret = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &raw);
        if (ret == LIBEVDEV_READ_STATUS_SUCCESS) {
            result.pushed = evdev_dispatch(input, device, &raw) || result.pushed;
        } else if (ret == LIBEVDEV_READ_STATUS_SYNC) {
            NativeEvdevDrainResult sync_result = evdev_drain_sync(input, device);
            result.pushed = sync_result.pushed || result.pushed;
            if (sync_result.remove) {
                result.remove = true;
                break;
            }
        } else if (ret == -EAGAIN) {
            break;
        } else {
            fprintf(stderr, "[native-input] libevdev read failed on fd %d: %s\n", device->fd, strerror(-ret));
            result.remove = true;
            break;
        }
    }
    return result;
}

static void evdev_close_device(NativeEvdevDevice *device) {
    if (!device) {
        return;
    }
    if (device->dev) {
        int ret = libevdev_grab(device->dev, LIBEVDEV_UNGRAB);
        if (ret < 0 && ret != -ENODEV) {
            fprintf(stderr, "[native-input] failed to ungrab fd %d: %s\n", device->fd, strerror(-ret));
        }
        libevdev_free(device->dev);
        device->dev = NULL;
    }
    if (device->fd >= 0) {
        close(device->fd);
        device->fd = -1;
    }
}

static void evdev_update_active_flags(NativeEvdevInput *input, const NativeEvdevBackend *backend) {
    bool mouse_active = false;
    bool keyboard_active = false;
    if (backend) {
        for (int i = 0; i < backend->ndevices; i++) {
            if (backend->devices[i].is_mouse) {
                mouse_active = true;
            }
            if (backend->devices[i].is_keyboard) {
                keyboard_active = true;
            }
        }
    }
    atomic_store(&input->mouse_active, mouse_active);
    atomic_store(&input->keyboard_active, keyboard_active);
}

static const char *evdev_device_kind_name(const NativeEvdevDevice *device) {
    if (device->is_mouse && device->is_keyboard) {
        return "keyboard+mouse";
    }
    if (device->is_mouse) {
        return "mouse";
    }
    if (device->is_keyboard) {
        return "keyboard";
    }
    return "unknown";
}

static void evdev_remove_device(NativeEvdevInput *input, NativeEvdevBackend *backend, int index) {
    if (!input || !backend || index < 0 || index >= backend->ndevices) {
        return;
    }
    NativeEvdevDevice removed = backend->devices[index];
    fprintf(stderr, "[native-input] removing %s fd %d after disconnect/error\n", evdev_device_kind_name(&removed),
            removed.fd);
    evdev_close_device(&removed);
    for (int i = index; i + 1 < backend->ndevices; i++) {
        backend->devices[i] = backend->devices[i + 1];
    }
    backend->ndevices--;
    backend->devices[backend->ndevices].fd = -1;
    backend->devices[backend->ndevices].dev = NULL;
    evdev_update_active_flags(input, backend);
}

/* Ungrab and close every open device (releasing EVIOCGRAB) and clear the active flags, WITHOUT
 * freeing the backend. Used by the reader thread when it gives up on a persistent poll() error,
 * so grabbed USB devices are actually handed back to the compositor (real SDL fallback) instead
 * of staying captured until session teardown. The backend struct and its inotify fd remain for
 * native_evdev_input_stop() to free after joining the thread. Idempotent (evdev_close_device
 * clears fd/dev), so a later evdev_close_backend() over 0 devices is a no-op. */
static void evdev_release_devices(NativeEvdevInput *input, NativeEvdevBackend *backend) {
    for (int i = 0; i < backend->ndevices; i++) {
        evdev_close_device(&backend->devices[i]);
    }
    backend->ndevices = 0;
    evdev_update_active_flags(input, backend);
}

static void evdev_close_backend(NativeEvdevBackend *backend) {
    if (!backend) {
        return;
    }
    for (int i = 0; i < backend->ndevices; i++) {
        evdev_close_device(&backend->devices[i]);
    }
    backend->ndevices = 0;
    if (backend->inotify_fd >= 0) {
        close(backend->inotify_fd);
        backend->inotify_fd = -1;
    }
    free(backend);
}

static bool evdev_backend_has_path(const NativeEvdevBackend *backend, const char *path) {
    for (int i = 0; i < backend->ndevices; i++) {
        if (strcmp(backend->devices[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool evdev_add_device(NativeEvdevInput *input, NativeEvdevBackend *backend, const char *path) {
    /* Already grabbed (initial scan or a prior hotplug rescan): nothing to do. Skipping before
     * open() also avoids a second EVIOCGRAB on a device we already hold, which would fail EBUSY
     * and leave a duplicate ungrabbed entry. */
    if (backend->ndevices >= NATIVE_EVDEV_MAX_DEVICES || evdev_backend_has_path(backend, path)) {
        return false;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENXIO && errno != EACCES) {
            fprintf(stderr, "[native-input] failed to open %s: %s\n", path, strerror(errno));
        }
        return false;
    }

    struct libevdev *dev = NULL;
    int ret = libevdev_new_from_fd(fd, &dev);
    if (ret < 0) {
        close(fd);
        return false;
    }

    bool is_keyboard = evdev_is_keyboard(dev);
    bool is_mouse = evdev_is_mouse(dev);
    if (!is_keyboard && !is_mouse) {
        libevdev_free(dev);
        close(fd);
        return false;
    }

    NativeEvdevDevice *device = &backend->devices[backend->ndevices];
    device->fd = fd;
    device->dev = dev;
    device->is_mouse = is_mouse;
    device->is_keyboard = is_keyboard;
    (void)snprintf(device->path, sizeof(device->path), "%s", path);
    const char *kind_name = evdev_device_kind_name(device);

    ret = libevdev_grab(dev, LIBEVDEV_GRAB);
    if (ret < 0) {
        fprintf(stderr, "[native-input] failed to grab %s %s: %s (kept ungrabbed)\n", kind_name, path, strerror(-ret));
    } else {
        fprintf(stderr, "[native-input] grabbed %s %s (%s)\n", kind_name, path, libevdev_get_name(dev));
    }

    backend->ndevices++;
    if (is_mouse) {
        atomic_store(&input->mouse_active, true);
    }
    if (is_keyboard) {
        atomic_store(&input->keyboard_active, true);
    }
    return true;
}

struct evdev_open_context {
    NativeEvdevInput *input;
    NativeEvdevBackend *backend;
};

static bool evdev_open_visit(const char *path, void *user) {
    struct evdev_open_context *ctx = (struct evdev_open_context *)user;
    (void)evdev_add_device(ctx->input, ctx->backend, path);
    return ctx->backend->ndevices >= NATIVE_EVDEV_MAX_DEVICES; /* stop once the table is full */
}

static NativeEvdevBackend *evdev_open_backend(NativeEvdevInput *input) {
    NativeEvdevBackend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        return NULL;
    }
    for (int i = 0; i < NATIVE_EVDEV_MAX_DEVICES; i++) {
        backend->devices[i].fd = -1;
    }

    /* Watch /dev/input so a keyboard/mouse plugged in mid-session is grabbed without a restart
     * (unplug is already handled by the reader's poll-error path). Best effort: if inotify is
     * unavailable we simply fall back to a one-shot scan. */
    backend->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (backend->inotify_fd >= 0 && inotify_add_watch(backend->inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        fprintf(stderr, "[native-input] inotify watch on /dev/input failed (%s); hotplug add disabled\n",
                strerror(errno));
        close(backend->inotify_fd);
        backend->inotify_fd = -1;
    }

    struct evdev_open_context ctx = {input, backend};
    (void)evdev_for_each_input_node(evdev_open_visit, &ctx);

    /* Keep the reader alive with zero devices as long as hotplug is watching, so "start
     * streaming, then plug in a keyboard" still works. Only give up when there is nothing to
     * read and no way to notice a later device. */
    if (backend->ndevices == 0 && backend->inotify_fd < 0) {
        evdev_close_backend(backend);
        return NULL;
    }
    return backend;
}

/* Drain the inotify queue (used only as a "something changed" trigger) and reconcile the
 * grabbed set against /dev/input, grabbing any node we do not already hold. A full rescan is
 * used rather than acting on individual inotify records so node renumbering across an
 * unplug/replug cannot slip through. Returns true if the device set changed. */
static bool evdev_handle_hotplug(NativeEvdevInput *input, NativeEvdevBackend *backend) {
    if (backend->inotify_fd < 0) {
        return false;
    }
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(backend->inotify_fd, buf, sizeof(buf)) > 0) {
        /* contents intentionally ignored; the rescan below is the source of truth */
    }

    int before = backend->ndevices;
    struct evdev_open_context ctx = {input, backend};
    (void)evdev_for_each_input_node(evdev_open_visit, &ctx);
    return backend->ndevices != before;
}

static void *evdev_thread(void *arg) {
    NativeEvdevInput *input = (NativeEvdevInput *)arg;
    NativeEvdevBackend *backend = (NativeEvdevBackend *)input->backend;
    /* +2 for the stop eventfd and the /dev/input inotify fd. */
    struct pollfd fds[NATIVE_EVDEV_MAX_DEVICES + 2];
    int poll_failures = 0;

    while (atomic_load(&input->running)) {
        fds[0].fd = input->stop_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = backend->inotify_fd; /* a negative fd is ignored by poll() */
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        for (int i = 0; i < backend->ndevices; i++) {
            fds[i + 2].fd = backend->devices[i].fd;
            fds[i + 2].events = POLLIN;
            fds[i + 2].revents = 0;
        }

        int ret = poll(fds, (nfds_t)(backend->ndevices + 2), -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[native-input] poll failed: %s\n", strerror(errno));
            /* A persistent poll error (not EINTR) would otherwise spin the reader at 100% CPU,
             * starving video decode and flooding the log. Back off briefly, and after a bounded
             * run of consecutive failures give up so the app drops back to SDL input. */
            if (++poll_failures >= NATIVE_EVDEV_MAX_POLL_FAILURES) {
                fprintf(stderr, "[native-input] too many consecutive poll failures; stopping evdev reader\n");
                /* Hand the devices back to the compositor before quitting, otherwise they stay
                 * EVIOCGRAB-captured and the SDL fallback would have nothing to read. */
                evdev_release_devices(input, backend);
                atomic_store(&input->running, false);
                break;
            }
            struct timespec backoff = {0, 10 * 1000 * 1000}; /* 10ms */
            (void)nanosleep(&backoff, NULL);
            continue;
        }
        poll_failures = 0;
        if (fds[0].revents & POLLIN) {
            uint64_t value;
            while (read(input->stop_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
            }
            break;
        }

        bool pushed = false;
        bool changed = false;
        /* Handle disconnects first so the hotplug rescan below sees an accurate device set
         * (e.g. an unplug/replug that reuses the same eventN node). */
        for (int i = backend->ndevices - 1; i >= 0; i--) {
            if (fds[i + 2].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
                NativeEvdevDrainResult drain = evdev_drain_device(input, &backend->devices[i]);
                pushed = drain.pushed || pushed;
                if (drain.remove || (fds[i + 2].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    evdev_remove_device(input, backend, i);
                    changed = true;
                }
            }
        }
        /* Then pick up devices connected since the last scan. */
        if (fds[1].revents & POLLIN) {
            changed = evdev_handle_hotplug(input, backend) || changed;
        }
        /* Wake the main loop on new input and on a device-set change so it re-reads the
         * mouse/keyboard active flags and starts (or stops) draining promptly. */
        if (pushed || changed) {
            evdev_notify_main(input);
        }
    }
    return NULL;
}

bool native_evdev_input_start(NativeEvdevInput *input) {
    if (!input || input->started) {
        return input && input->started;
    }
    memset(input, 0, sizeof(*input));
    input->wake_fd = -1;
    input->stop_fd = -1;
    pthread_mutex_init(&input->lock, NULL);
    input->lock_initialized = true;
    atomic_init(&input->running, false);
    atomic_init(&input->mouse_active, false);
    atomic_init(&input->keyboard_active, false);
    atomic_init(&input->event_count, 0u);

    input->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    input->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (input->wake_fd < 0 || input->stop_fd < 0) {
        fprintf(stderr, "[native-input] eventfd() failed; evdev input is unavailable: %s\n", strerror(errno));
        native_evdev_input_stop(input);
        return false;
    }

    NativeEvdevBackend *backend = evdev_open_backend(input);
    if (!backend) {
        fprintf(stderr, "[native-input] no USB mouse/keyboard devices found for evdev grab\n");
        native_evdev_input_stop(input);
        return false;
    }
    input->backend = backend;

    atomic_store(&input->running, true);
    if (pthread_create(&input->thread, NULL, evdev_thread, input) != 0) {
        fprintf(stderr, "[native-input] failed to start evdev reader thread\n");
        atomic_store(&input->running, false);
        native_evdev_input_stop(input);
        return false;
    }
    input->started = true;
    fprintf(stderr, "[native-input] libevdev reader started (%d device(s), mouse=%d keyboard=%d)\n", backend->ndevices,
            atomic_load(&input->mouse_active) ? 1 : 0, atomic_load(&input->keyboard_active) ? 1 : 0);
    return true;
}

void native_evdev_input_stop(NativeEvdevInput *input) {
    if (!input) {
        return;
    }
    if (!input->lock_initialized) {
        /* App is zero-initialized, so wake_fd/stop_fd are 0 until start() sets them to -1.
         * A stop before start must not close fd 0 or whatever resource reused it. */
        return;
    }
    if (input->started) {
        atomic_store(&input->running, false);
        if (input->stop_fd >= 0) {
            uint64_t one = 1;
            (void)write(input->stop_fd, &one, sizeof(one));
        }
        pthread_join(input->thread, NULL);
    }

    if (input->backend) {
        evdev_close_backend((NativeEvdevBackend *)input->backend);
        input->backend = NULL;
    }
    if (input->wake_fd >= 0) {
        close(input->wake_fd);
        input->wake_fd = -1;
    }
    if (input->stop_fd >= 0) {
        close(input->stop_fd);
        input->stop_fd = -1;
    }
    if (input->lock_initialized) {
        pthread_mutex_destroy(&input->lock);
        input->lock_initialized = false;
    }
    input->started = false;
    atomic_store(&input->mouse_active, false);
    atomic_store(&input->keyboard_active, false);
}

bool native_evdev_input_active(const NativeEvdevInput *input) {
    return input && input->started && atomic_load(&input->running);
}

static bool evdev_mouse_queue_pending(NativeEvdevInput *input) {
    bool pending;
    pthread_mutex_lock(&input->lock);
    pending = input->mouse_head != input->mouse_tail;
    pthread_mutex_unlock(&input->lock);
    return pending;
}

static bool evdev_keyboard_queue_pending(NativeEvdevInput *input) {
    bool pending;
    pthread_mutex_lock(&input->lock);
    pending = input->keyboard_head != input->keyboard_tail;
    pthread_mutex_unlock(&input->lock);
    return pending;
}

bool native_evdev_input_mouse_active(NativeEvdevInput *input) {
    return native_evdev_input_active(input) &&
           (atomic_load(&input->mouse_active) || evdev_mouse_queue_pending(input));
}

bool native_evdev_input_keyboard_active(NativeEvdevInput *input) {
    return native_evdev_input_active(input) &&
           (atomic_load(&input->keyboard_active) || evdev_keyboard_queue_pending(input));
}

int native_evdev_input_wake_fd(const NativeEvdevInput *input) {
    if (!native_evdev_input_active(input) || input->wake_fd < 0) {
        return -1;
    }
    return input->wake_fd;
}

void native_evdev_input_clear_wake(NativeEvdevInput *input) {
    if (!input || input->wake_fd < 0) {
        return;
    }
    uint64_t value;
    while (read(input->wake_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
    }
}

size_t native_evdev_input_pop_mouse_batch(NativeEvdevInput *input, NativeMouseEv *out, size_t cap) {
    if (!input || !input->started || !out || cap == 0) {
        return 0;
    }
    size_t count = 0;
    pthread_mutex_lock(&input->lock);
    while (input->mouse_head != input->mouse_tail && count < cap) {
        out[count++] = input->mouse_ring[input->mouse_head];
        input->mouse_head = (input->mouse_head + 1u) % NATIVE_EVDEV_MOUSE_RING;
    }
    pthread_mutex_unlock(&input->lock);
    return count;
}

size_t native_evdev_input_pop_keyboard_batch(NativeEvdevInput *input, NativeKeyboardEv *out, size_t cap) {
    if (!input || !input->started || !out || cap == 0) {
        return 0;
    }
    size_t count = 0;
    pthread_mutex_lock(&input->lock);
    while (input->keyboard_head != input->keyboard_tail && count < cap) {
        out[count++] = input->keyboard_ring[input->keyboard_head];
        input->keyboard_head = (input->keyboard_head + 1u) % NATIVE_EVDEV_KEYBOARD_RING;
    }
    pthread_mutex_unlock(&input->lock);
    return count;
}
