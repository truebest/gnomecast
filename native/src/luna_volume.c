#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "luna_volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool native_luna_volume_parse(const char *json, int *volume, bool *muted) {
    if (!json) {
        return false;
    }
    /* Replies are single-line flat JSON from a system service; field scanning is enough
     * (and keeps a JSON library out of the build). An explicit failure beats guessing. */
    if (strstr(json, "\"returnValue\":false")) {
        return false;
    }
    const char *field = strstr(json, "\"volume\":");
    if (!field) {
        return false;
    }
    char *end = NULL;
    long value = strtol(field + strlen("\"volume\":"), &end, 10);
    if (end == field + strlen("\"volume\":") || value < 0 || value > 100) {
        return false;
    }
    if (volume) {
        *volume = (int)value;
    }
    if (muted) {
        *muted = strstr(json, "\"muteStatus\":true") != NULL;
    }
    return true;
}

int native_luna_volume_cached(const NativeLunaVolume *lv) {
    return lv ? atomic_load(&lv->volume_pct) : -1;
}

bool native_luna_volume_available(const NativeLunaVolume *lv) {
    return lv && atomic_load(&lv->available);
}

unsigned native_luna_volume_reply_seq(const NativeLunaVolume *lv) {
    return lv ? atomic_load(&lv->reply_seq) : 0u;
}

#if defined(HELLOLG_WITH_LS2) && HELLOLG_WITH_LS2

#include <glib.h>
#include <luna-service2/lunaservice.h>
#include <pthread.h>

typedef struct NativeLunaVolumeImpl {
    NativeLunaVolume *owner;
    pthread_t thread;
    bool thread_running;
    GMainContext *context;
    GMainLoop *loop;
    LSHandle *handle;
    /* Coalesced pending set: the newest requested value; -1 = none. The invoke flag
     * keeps at most one dispatch queued into the loop thread however fast the fader
     * moves. */
    atomic_int desired_pct;
    atomic_bool invoke_queued;
    atomic_bool loop_ready;
} NativeLunaVolumeImpl;

/* Shared reply handler: one-shot gets, set confirmations and every subscription event
 * all carry the same payload shape. Loop thread. */
static bool luna_volume_reply_cb(LSHandle *sh, LSMessage *message, void *ctx) {
    (void)sh;
    NativeLunaVolume *lv = (NativeLunaVolume *)ctx;
    const char *payload = message ? LSMessageGetPayload(message) : NULL;
    int volume = -1;
    bool muted = false;
    if (payload && native_luna_volume_parse(payload, &volume, &muted)) {
        atomic_store(&lv->volume_pct, volume);
        atomic_store(&lv->muted, muted);
        atomic_store(&lv->available, true);
        /* After the stores: a reader that saw the bump also sees the fresh volume. */
        atomic_fetch_add(&lv->reply_seq, 1u);
    }
    return true;
}

/* Loop thread. Sends the newest coalesced set (if any); the reply flows through the
 * shared handler. */
static gboolean luna_volume_dispatch_cb(gpointer data) {
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)data;
    atomic_store(&impl->invoke_queued, false);
    int pct = atomic_exchange(&impl->desired_pct, -1);
    if (pct >= 0 && impl->handle) {
        char payload[48];
        (void)snprintf(payload, sizeof(payload), "{\"volume\":%d}", pct);
        LSError error;
        LSErrorInit(&error);
        if (!LSCallOneReply(impl->handle, "luna://com.webos.audio/setVolume", payload, luna_volume_reply_cb,
                            impl->owner, NULL, &error)) {
            fprintf(stderr, "[native-luna] setVolume call failed: %s\n", error.message ? error.message : "?");
            LSErrorFree(&error);
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean luna_volume_refresh_cb(gpointer data) {
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)data;
    if (impl->handle) {
        LSError error;
        LSErrorInit(&error);
        if (!LSCallOneReply(impl->handle, "luna://com.webos.audio/getVolume", "{}", luna_volume_reply_cb,
                            impl->owner, NULL, &error)) {
            fprintf(stderr, "[native-luna] getVolume call failed: %s\n", error.message ? error.message : "?");
            LSErrorFree(&error);
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean luna_volume_quit_cb(gpointer data) {
    g_main_loop_quit((GMainLoop *)data);
    return G_SOURCE_REMOVE;
}

static void *luna_volume_thread(void *arg) {
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)arg;
    impl->context = g_main_context_new();
    g_main_context_push_thread_default(impl->context);
    impl->loop = g_main_loop_new(impl->context, FALSE);

    LSError error;
    LSErrorInit(&error);
    /* Anonymous client — the ONLY identity the dev-mode jail permits (verified live on
     * webOS 24: a named LSRegister answers "Invalid permissions"). */
    if (!LSRegister(NULL, &impl->handle, &error)) {
        fprintf(stderr, "[native-luna] LSRegister failed: %s\n", error.message ? error.message : "?");
        LSErrorFree(&error);
        impl->handle = NULL;
    } else if (!LSGmainAttach(impl->handle, impl->loop, &error)) {
        fprintf(stderr, "[native-luna] LSGmainAttach failed: %s\n", error.message ? error.message : "?");
        LSErrorFree(&error);
        LSErrorInit(&error);
        (void)LSUnregister(impl->handle, &error);
        LSErrorFree(&error);
        impl->handle = NULL;
    }

    /* NO volume-change subscription — every path was probed live on webOS 24 and none
     * delivers events to a dev-mode app, so callers poll getVolume instead (cheap
     * in-process one-shots that also wake the dozing service). The dead ends, with
     * their exact refusals, so nobody re-walks them:
     *   - com.webos.audio/getVolume subscribe: accepted ("subscribed":true) but the
     *     service is DYNAMIC — the hub launches it per request and it idles out, taking
     *     the subscription with it ("com.webos.audio is not running"); zero change
     *     events arrived even while it was alive.
     *   - com.webos.service.audio/master/getVolume subscribe (the change-notifying one
     *     in the webOS OSE docs): "Message status unknown" for this client.
     *   - com.webos.service.apiadapter/audio/getVolume (backs the external SSAP
     *     clients, which DO receive change events): "Not permitted to send" — that
     *     route is for paired external controllers only.
     *   - Registering a NAMED client to widen the ACLs: "Invalid permissions" — the
     *     jail allows anonymous registration only. */

    atomic_store(&impl->loop_ready, true);
    g_main_loop_run(impl->loop);

    if (impl->handle) {
        LSErrorInit(&error);
        (void)LSUnregister(impl->handle, &error); /* cancels in-flight calls and the subscription */
        LSErrorFree(&error);
        impl->handle = NULL;
    }
    g_main_loop_unref(impl->loop);
    g_main_context_pop_thread_default(impl->context);
    g_main_context_unref(impl->context);
    return NULL;
}

bool native_luna_volume_start(NativeLunaVolume *lv) {
    if (!lv || lv->impl) {
        return false;
    }
    memset(lv, 0, sizeof(*lv));
    atomic_init(&lv->volume_pct, -1);
    atomic_init(&lv->muted, false);
    atomic_init(&lv->available, false);
    atomic_init(&lv->reply_seq, 0u);
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        return false;
    }
    impl->owner = lv;
    atomic_init(&impl->desired_pct, -1);
    atomic_init(&impl->invoke_queued, false);
    atomic_init(&impl->loop_ready, false);
    if (pthread_create(&impl->thread, NULL, luna_volume_thread, impl) != 0) {
        free(impl);
        return false;
    }
    impl->thread_running = true;
    lv->impl = impl;
    return true;
}

void native_luna_volume_stop(NativeLunaVolume *lv) {
    if (!lv || !lv->impl) {
        return;
    }
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)lv->impl;
    if (impl->thread_running) {
        /* The loop may still be booting; the idle source runs once it is up either way. */
        while (!atomic_load(&impl->loop_ready)) {
            g_usleep(1000);
        }
        GSource *source = g_idle_source_new();
        g_source_set_callback(source, luna_volume_quit_cb, impl->loop, NULL);
        g_source_attach(source, impl->context);
        g_source_unref(source);
        pthread_join(impl->thread, NULL);
        impl->thread_running = false;
    }
    free(impl);
    lv->impl = NULL;
}

/* Queue `cb` into the loop thread (any thread). False while the loop is still booting
 * (the boot path already reads the volume via the subscription's first reply). */
static bool luna_volume_invoke(NativeLunaVolumeImpl *impl, GSourceFunc cb) {
    if (!atomic_load(&impl->loop_ready)) {
        return false;
    }
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, cb, impl, NULL);
    g_source_attach(source, impl->context);
    g_source_unref(source);
    return true;
}

void native_luna_volume_refresh(NativeLunaVolume *lv) {
    if (!lv || !lv->impl) {
        return;
    }
    (void)luna_volume_invoke((NativeLunaVolumeImpl *)lv->impl, luna_volume_refresh_cb);
}

void native_luna_volume_set(NativeLunaVolume *lv, int pct) {
    if (!lv || !lv->impl) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    /* Optimistic cache: the knob must track key presses immediately, not at bus pace. */
    atomic_store(&lv->volume_pct, pct);
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)lv->impl;
    atomic_store(&impl->desired_pct, pct);
    if (!atomic_exchange(&impl->invoke_queued, true)) {
        if (!luna_volume_invoke(impl, luna_volume_dispatch_cb)) {
            atomic_store(&impl->invoke_queued, false); /* loop still booting: drop, retry on next set */
        }
    }
}

#else /* !HELLOLG_WITH_LS2: host builds — no Luna bus, the fader just stays dimmed */

bool native_luna_volume_start(NativeLunaVolume *lv) {
    if (!lv) {
        return false;
    }
    memset(lv, 0, sizeof(*lv));
    atomic_init(&lv->volume_pct, -1);
    atomic_init(&lv->muted, false);
    atomic_init(&lv->available, false);
    atomic_init(&lv->reply_seq, 0u);
    return true;
}

void native_luna_volume_stop(NativeLunaVolume *lv) {
    (void)lv;
}

void native_luna_volume_refresh(NativeLunaVolume *lv) {
    (void)lv;
}

void native_luna_volume_set(NativeLunaVolume *lv, int pct) {
    (void)lv;
    (void)pct;
}

#endif /* HELLOLG_WITH_LS2 */
