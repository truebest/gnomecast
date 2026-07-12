#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "luna_volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HELLOLG_WITH_LS2) && HELLOLG_WITH_LS2
#include <glib.h>
#include <luna-service2/lunaservice.h>
#include <pthread.h>
#endif

#include "clog.h"

clog_define(g_native_log_luna, cLogLevelInfo, cLogFlags_Default, "luna.volume", NULL);

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
    /* Flow control, loop thread only (every LS2 callback runs on the loop's context).
     * At most one setVolume and one getVolume are in flight at a time: the bus replies
     * far slower than remote-key autorepeat or the 200ms poll, and unthrottled calls
     * pile up outstanding callback records and answer out of order. Tokens let a
     * stalled call be CANCELLED before its replacement goes out — and let callbacks
     * ignore a superseded call's late reply. */
    bool set_in_flight;
    bool refresh_in_flight;
    gint64 refresh_issued_us;
    gint64 set_issued_us;
    LSMessageToken get_token;
    LSMessageToken set_token;
} NativeLunaVolumeImpl;

/* A reply never comes back for a lost call when the service hangs while staying
 * registered (a DEAD service is answered by the hub itself with an error reply); the
 * stall ceiling re-arms polling/sets instead of wedging them forever. */
#define LUNA_VOLUME_CALL_STALL_US (2 * G_USEC_PER_SEC)

/* Cancels a stalled call's callback record so it cannot pile up per stall window or
 * fire late over a newer call's result. Loop thread. */
static void luna_volume_cancel_call(NativeLunaVolumeImpl *impl, LSMessageToken token) {
    LSError error;
    LSErrorInit(&error);
    if (!LSCallCancel(impl->handle, token, &error)) {
        LSErrorFree(&error);
    }
}

/* getVolume replies: the ONLY authoritative cache writer. Loop thread. */
static bool luna_volume_get_reply_cb(LSHandle *sh, LSMessage *message, void *ctx) {
    (void)sh;
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)ctx;
    NativeLunaVolume *lv = impl->owner;
    if (message && LSMessageGetResponseToken(message) != impl->get_token) {
        /* A superseded call's late reply (stalled calls are cancelled, this is the
         * belt): it may neither write an older reading nor re-arm the poll. */
        return true;
    }
    impl->refresh_in_flight = false;
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

/* Sends the newest coalesced set, if any. Loop thread. */
static void luna_volume_send_set(NativeLunaVolumeImpl *impl);

/* setVolume replies deliberately touch NEITHER the volume cache NOR availability: the
 * confirmation echoes the value of an OLDER call than the newest optimistic store
 * whenever sets are chained (applying it would visibly yank the knob backward), and a
 * reply also arrives for a REJECTED set (returnValue:false) — lighting the dim MASTER
 * off one would present an optimistic value the TV never accepted. getVolume replies
 * are the sole authority for both; on firmware that rejects these endpoints the fader
 * stays dim, as the header promises. Loop thread. */
static bool luna_volume_set_reply_cb(LSHandle *sh, LSMessage *message, void *ctx) {
    (void)sh;
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)ctx;
    if (message && LSMessageGetResponseToken(message) != impl->set_token) {
        return true; /* a superseded call's late reply must not clear the new in-flight */
    }
    impl->set_in_flight = false;
    if (atomic_load(&impl->desired_pct) >= 0) {
        luna_volume_send_set(impl); /* chain the newest value the drag left behind */
    }
    return true;
}

static void luna_volume_send_set(NativeLunaVolumeImpl *impl) {
    int pct = atomic_exchange(&impl->desired_pct, -1);
    if (pct < 0 || !impl->handle) {
        return;
    }
    char payload[48];
    (void)snprintf(payload, sizeof(payload), "{\"volume\":%d}", pct);
    LSError error;
    LSErrorInit(&error);
    if (!LSCallOneReply(impl->handle, "luna://com.webos.audio/setVolume", payload, luna_volume_set_reply_cb, impl,
                        &impl->set_token, &error)) {
        clog(cLogLevelWarning, "setVolume call failed: %s", error.message ? error.message : "?");
        LSErrorFree(&error);
        /* Not in flight; the next set (or the poll) recovers the knob. */
        return;
    }
    impl->set_in_flight = true;
    impl->set_issued_us = g_get_monotonic_time();
}

/* Loop thread. Starts a set only when none is in flight — otherwise the pending value
 * waits for the in-flight call's reply to chain it (one call in flight, ever). A set
 * whose reply never came (hung-but-registered service) is cancelled after the stall
 * ceiling: without that the MASTER wedges forever, because later fader edits only
 * replace desired_pct and this guard would never dispatch them. */
static gboolean luna_volume_dispatch_cb(gpointer data) {
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)data;
    atomic_store(&impl->invoke_queued, false);
    if (impl->set_in_flight) {
        if (g_get_monotonic_time() - impl->set_issued_us < LUNA_VOLUME_CALL_STALL_US) {
            return G_SOURCE_REMOVE; /* the reply (or the next edit after the ceiling) chains it */
        }
        luna_volume_cancel_call(impl, impl->set_token);
        impl->set_in_flight = false;
    }
    luna_volume_send_set(impl);
    return G_SOURCE_REMOVE;
}

static gboolean luna_volume_refresh_cb(gpointer data) {
    NativeLunaVolumeImpl *impl = (NativeLunaVolumeImpl *)data;
    if (!impl->handle) {
        return G_SOURCE_REMOVE;
    }
    gint64 now = g_get_monotonic_time();
    /* The 200ms poll is this module's only steady heartbeat, so stalled SETS recover
     * here too: dispatch_cb only runs on user edits, and an edit landing before the
     * stall ceiling leaves desired_pct waiting for a chain reply that never comes —
     * the newest requested volume would stay unsent until the next edit. */
    if (impl->set_in_flight && now - impl->set_issued_us >= LUNA_VOLUME_CALL_STALL_US) {
        luna_volume_cancel_call(impl, impl->set_token);
        impl->set_in_flight = false;
        luna_volume_send_set(impl);
    }
    if (impl->refresh_in_flight) {
        if (now - impl->refresh_issued_us < LUNA_VOLUME_CALL_STALL_US) {
            return G_SOURCE_REMOVE; /* one outstanding get is enough; the poll re-arms after its reply */
        }
        /* Never answered: cancel the record BEFORE replacing the call, or a hung
         * service accumulates one per stall window and its eventual late reply would
         * overwrite a newer reading. */
        luna_volume_cancel_call(impl, impl->get_token);
        impl->refresh_in_flight = false;
    }
    LSError error;
    LSErrorInit(&error);
    if (!LSCallOneReply(impl->handle, "luna://com.webos.audio/getVolume", "{}", luna_volume_get_reply_cb, impl,
                        &impl->get_token, &error)) {
        clog(cLogLevelWarning, "getVolume call failed: %s", error.message ? error.message : "?");
        LSErrorFree(&error);
        return G_SOURCE_REMOVE;
    }
    impl->refresh_in_flight = true;
    impl->refresh_issued_us = now;
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
    /* Anonymous client — the ONLY identity the dev-mode jail permits (verified live:
     * a named LSRegister answers "Invalid permissions"). */
    if (!LSRegister(NULL, &impl->handle, &error)) {
        clog(cLogLevelWarning, "LSRegister failed: %s", error.message ? error.message : "?");
        LSErrorFree(&error);
        impl->handle = NULL;
    } else if (!LSGmainAttach(impl->handle, impl->loop, &error)) {
        clog(cLogLevelWarning, "LSGmainAttach failed: %s", error.message ? error.message : "?");
        LSErrorFree(&error);
        LSErrorInit(&error);
        (void)LSUnregister(impl->handle, &error);
        LSErrorFree(&error);
        impl->handle = NULL;
    }

    /* NO volume-change subscription — every path was probed live on the target
     * firmware and none delivers events to a dev-mode app, so callers poll getVolume
     * instead (cheap
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

/* Queue `cb` into the loop thread (any thread). False while the loop is still booting —
 * the queued work is simply dropped; the 200ms poll re-issues reads soon enough. */
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
