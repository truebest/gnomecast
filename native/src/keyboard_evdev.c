#include "keyboard_evdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#include <linux/input.h>

#define NATIVE_KEYBOARD_MAX_FDS 8

static bool has_bit(const unsigned long *bits, int bit) {
    return (bits[bit / (8 * (int)sizeof(long))] >> (bit % (8 * (int)sizeof(long)))) & 1ul;
}

/* A real typing keyboard: has the alphabetic block and space, and is not a mouse. This
 * excludes the TV remote (no letter keys) and the mouse (BTN_LEFT), so only the attached
 * USB keyboard is grabbed. */
static bool device_is_keyboard(int fd) {
    unsigned long keybits[(KEY_MAX / (8 * sizeof(long))) + 1];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
        return false;
    }
    if (has_bit(keybits, BTN_LEFT) || has_bit(keybits, BTN_MOUSE)) {
        return false;
    }
    return has_bit(keybits, KEY_A) && has_bit(keybits, KEY_Z) && has_bit(keybits, KEY_SPACE);
}

bool native_keyboard_evdev_probe(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[300];
        (void)snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        found = device_is_keyboard(fd); /* caps check only; no grab */
        close(fd);
    }
    closedir(dir);
    return found;
}

static int keyboard_open_devices(int *fds, int max_fds) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return 0;
    }
    int nfds = 0;
    struct dirent *ent;
    while (nfds < max_fds && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[300];
        (void)snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        if (device_is_keyboard(fd)) {
            if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                fprintf(stderr, "[native-keyboard] failed to grab %s: %s (kept ungrabbed)\n", path,
                        strerror(errno));
            } else {
                fprintf(stderr, "[native-keyboard] grabbed keyboard %s\n", path);
            }
            fds[nfds++] = fd;
        } else {
            close(fd);
        }
    }
    closedir(dir);
    return nfds;
}

static void keyboard_push(NativeKeyboardEvdev *k, uint16_t code, bool down) {
    pthread_mutex_lock(&k->lock);
    unsigned next = (k->tail + 1u) % NATIVE_KEYBOARD_EV_RING;
    if (next == k->head) {
        k->head = (k->head + 1u) % NATIVE_KEYBOARD_EV_RING; /* drop oldest under overflow */
    }
    k->ring[k->tail].code = code;
    k->ring[k->tail].down = down;
    k->tail = next;
    pthread_mutex_unlock(&k->lock);
}

static void *keyboard_evdev_thread(void *arg) {
    NativeKeyboardEvdev *k = (NativeKeyboardEvdev *)arg;
    while (atomic_load(&k->running)) {
        fd_set set;
        FD_ZERO(&set);
        int maxfd = k->wake_pipe[0];
        FD_SET(k->wake_pipe[0], &set);
        for (int i = 0; i < k->nfds; i++) {
            FD_SET(k->fds[i], &set);
            if (k->fds[i] > maxfd) {
                maxfd = k->fds[i];
            }
        }
        if (select(maxfd + 1, &set, NULL, NULL, NULL) <= 0) {
            continue;
        }
        if (FD_ISSET(k->wake_pipe[0], &set)) {
            char drain[64];
            while (read(k->wake_pipe[0], drain, sizeof(drain)) > 0) {
            }
            continue; /* re-check running */
        }
        for (int i = 0; i < k->nfds; i++) {
            if (!FD_ISSET(k->fds[i], &set)) {
                continue;
            }
            struct input_event ev;
            ssize_t n = read(k->fds[i], &ev, sizeof(ev));
            if (n != (ssize_t)sizeof(ev)) {
                continue;
            }
            if (ev.type == EV_KEY) {
                /* value 1 = press, 2 = autorepeat (forward as a repeated press), 0 = release. */
                keyboard_push(k, ev.code, ev.value != 0);
            }
        }
    }
    return NULL;
}

bool native_keyboard_evdev_start(NativeKeyboardEvdev *k) {
    if (!k || k->started) {
        return k && k->started;
    }
    memset(k, 0, sizeof(*k));
    k->wake_pipe[0] = -1;
    k->wake_pipe[1] = -1;

    int fds[NATIVE_KEYBOARD_MAX_FDS];
    int nfds = keyboard_open_devices(fds, NATIVE_KEYBOARD_MAX_FDS);
    if (nfds == 0) {
        fprintf(stderr, "[native-keyboard] no USB keyboard found; keyboard input is unavailable (no SDL fallback)\n");
        return false;
    }
    if (pipe(k->wake_pipe) != 0) {
        fprintf(stderr, "[native-keyboard] pipe() failed; keyboard input is unavailable\n");
        for (int i = 0; i < nfds; i++) {
            (void)ioctl(fds[i], EVIOCGRAB, 0);
            close(fds[i]);
        }
        return false;
    }
    (void)fcntl(k->wake_pipe[0], F_SETFL, O_NONBLOCK);
    for (int i = 0; i < nfds; i++) {
        k->fds[i] = fds[i];
    }
    k->nfds = nfds;
    pthread_mutex_init(&k->lock, NULL);
    atomic_init(&k->running, true);
    if (pthread_create(&k->thread, NULL, keyboard_evdev_thread, k) != 0) {
        fprintf(stderr, "[native-keyboard] failed to start reader thread; keyboard input is unavailable\n");
        atomic_store(&k->running, false);
        for (int i = 0; i < k->nfds; i++) {
            (void)ioctl(k->fds[i], EVIOCGRAB, 0);
            close(k->fds[i]);
        }
        close(k->wake_pipe[0]);
        close(k->wake_pipe[1]);
        pthread_mutex_destroy(&k->lock);
        return false;
    }
    k->started = true;
    fprintf(stderr, "[native-keyboard] evdev keyboard reader started (%d device(s), grabbed)\n", nfds);
    return true;
}

void native_keyboard_evdev_stop(NativeKeyboardEvdev *k) {
    if (!k || !k->started) {
        return;
    }
    atomic_store(&k->running, false);
    if (k->wake_pipe[1] >= 0) {
        (void)write(k->wake_pipe[1], "x", 1); /* break the select() */
    }
    pthread_join(k->thread, NULL);
    for (int i = 0; i < k->nfds; i++) {
        (void)ioctl(k->fds[i], EVIOCGRAB, 0);
        close(k->fds[i]);
    }
    k->nfds = 0;
    if (k->wake_pipe[0] >= 0) {
        close(k->wake_pipe[0]);
    }
    if (k->wake_pipe[1] >= 0) {
        close(k->wake_pipe[1]);
    }
    k->wake_pipe[0] = -1;
    k->wake_pipe[1] = -1;
    pthread_mutex_destroy(&k->lock);
    k->started = false;
}

bool native_keyboard_evdev_active(const NativeKeyboardEvdev *k) {
    return k && k->started && atomic_load(&k->running);
}

bool native_keyboard_evdev_pop(NativeKeyboardEvdev *k, NativeKeyboardEv *out) {
    if (!k || !k->started || !out) {
        return false;
    }
    bool got = false;
    pthread_mutex_lock(&k->lock);
    if (k->head != k->tail) {
        *out = k->ring[k->head];
        k->head = (k->head + 1u) % NATIVE_KEYBOARD_EV_RING;
        got = true;
    }
    pthread_mutex_unlock(&k->lock);
    return got;
}
