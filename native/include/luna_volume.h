#ifndef GNOMECAST_LUNA_VOLUME_H
#define GNOMECAST_LUNA_VOLUME_H

#include <stdatomic.h>
#include <stdbool.h>

/* System (TV) master volume over the webOS Luna bus, for the mixer overlay's MASTER
 * fader. This deliberately does NOT touch the app's own audio mix: the fader mirrors
 * and drives the platform volume — the same one the remote's VOL keys, Bluetooth
 * headphone buttons and a keyboard's Fn-volume keys adjust — which webOS handles above
 * the app (none of those keys ever reach us; verified live).
 *
 * Transport: the NATIVE luna-service2 client library on a dedicated GMainLoop thread.
 * NEVER a luna-send-pub subprocess: fork()ing this process while the NDL/OMX video
 * pipeline is (re)loading raced its in-process decoder state and produced black
 * screens (child inherits driver/SVP handles), and a pipe-spawned subscriber cannot
 * deliver events anyway (block-buffered stdout, no stdbuf on the TV). In-process LS2
 * calls carry no fork. Volume-change SUBSCRIPTIONS do not exist for a dev-mode app on
 * webOS 24 — every endpoint was probed live and refused or went dark (the exact dead
 * ends are documented in luna_volume.c) — so callers keep the cache live by polling
 * getVolume: the one-shots are cheap and each call wakes the dozing dynamic service.
 *
 * The audio methods used (absolute get/setVolume on luna://com.webos.audio) are
 * undocumented-but-working from the app's jail (verified live; LG documents only
 * volumeUp/Down/setMuted for third parties). If a firmware ever rejects them, the
 * module just stays unavailable and the MASTER fader dims — nothing else breaks.
 *
 * set() coalesces: the loop thread always sends the LATEST requested value, so a fader
 * drag produces a bounded trickle of bus calls. The cache updates optimistically on
 * set() (snappy knob) and authoritatively from get replies. */

typedef struct NativeLunaVolume {
    void *impl; /* transport state (LS2 loop thread on webOS; NULL on host builds) */
    /* Results (atomics: read from the SDL render path every frame). */
    atomic_int volume_pct; /* 0..100; -1 = unknown/unavailable */
    atomic_bool muted;
    atomic_bool available;  /* the bus answered at least once */
    atomic_uint reply_seq;  /* bumps on every parsed reply — freshness fence for baselines */
} NativeLunaVolume;

/* Spawns the bus thread and the volume-change subscription. On platforms without the
 * Luna bus this is a no-op that leaves the state unavailable. */
bool native_luna_volume_start(NativeLunaVolume *lv);
void native_luna_volume_stop(NativeLunaVolume *lv);

/* Queue an asynchronous getVolume (belt for the subscription, e.g. on overlay open). */
void native_luna_volume_refresh(NativeLunaVolume *lv);

/* Queue an asynchronous setVolume of the newest value (0..100, clamped); updates the
 * cached volume optimistically so the UI tracks the knob immediately. */
void native_luna_volume_set(NativeLunaVolume *lv, int pct);

/* Cached system volume: 0..100, or -1 while unknown/unavailable. */
int native_luna_volume_cached(const NativeLunaVolume *lv);
bool native_luna_volume_available(const NativeLunaVolume *lv);

/* Count of bus replies parsed so far. Callers that must not act on a possibly stale
 * cache (e.g. the auto-raise baseline at streaming entry) record this value and wait
 * for it to move — the cache is only trusted once a reply arrived AFTER the record. */
unsigned native_luna_volume_reply_seq(const NativeLunaVolume *lv);

/* Pure reply parser (unit-tested): extracts "volume" and "muteStatus" from a Luna JSON
 * reply. Returns false when the reply carries no usable volume (returnValue false,
 * malformed, or the field is absent). */
bool native_luna_volume_parse(const char *json, int *volume, bool *muted);

#endif
