#ifndef GNOMECAST_AU_SNAPSHOT_H
#define GNOMECAST_AU_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compressed-AU snapshot of one RDP session's video stream, used to re-enter a server's
 * H.264 delta chain without a reconnect. gnome-remote-desktop emits exactly one IDR per
 * connection (no keyframe on suppress resume, Refresh Rect rejected), so once the shared
 * hardware decoder moves to another session, a backgrounded slot's stream becomes
 * undecodable and switching back costs a full on-screen reconnect. Instead the slot is
 * reconnected while it is INVISIBLE (backgrounded): the fresh connection's IDR — plus
 * the few deltas that race the suppress request — is cached here as the raw compressed
 * bytes that came off the wire, and the server is then suppressed. Nothing further is
 * transmitted after the cached AUs, so the server's first delta after resume references
 * exactly the state a replay of this snapshot rebuilds in the decoder.
 *
 * Nothing is decoded or transcoded locally; the snapshot is a byte-for-byte replay
 * source. A keyframe restarts the snapshot (only the newest IDR group can seed a
 * decoder), deltas before the first keyframe are ignored, and overflow — e.g. a server
 * that keeps streaming despite suppress — voids the whole snapshot, because a partial
 * delta chain is undecodable.
 *
 * Thread model: the caller serializes all access (in the native app: app->video_lock).
 */

/* One 4K IDR is ~1-5MB; the cap covers it plus the raced-delta tail with margin.
 * The buffer is allocated whole at arm time (virtual; pages commit as AUs land). */
#define NATIVE_AU_SNAPSHOT_MAX_BYTES (8u * 1024u * 1024u)
#define NATIVE_AU_SNAPSHOT_MAX_AUS 512u

typedef struct NativeAuSnapshotEntry {
    size_t offset; /* into NativeAuSnapshot.buf */
    size_t len;
    bool is_keyframe;
    uint64_t pts90k;
} NativeAuSnapshotEntry;

typedef struct NativeAuSnapshot {
    uint8_t *buf; /* NATIVE_AU_SNAPSHOT_MAX_BYTES once armed; NULL otherwise */
    size_t used;
    NativeAuSnapshotEntry entries[NATIVE_AU_SNAPSHOT_MAX_AUS];
    unsigned count;
    bool armed;   /* appends are accepted */
    bool has_idr; /* a keyframe seeded the snapshot */
} NativeAuSnapshot;

/* Empties the snapshot and (re)allocates the byte buffer; appends are accepted until the
 * snapshot is reset or overflows. Returns false when the allocation fails (the snapshot
 * stays disarmed). */
bool native_au_snapshot_arm(NativeAuSnapshot *snap);

/* Drops everything and frees the buffer. Safe on a zeroed or already-reset snapshot. */
void native_au_snapshot_reset(NativeAuSnapshot *snap);

/* Caches one compressed AU. Keyframes restart the snapshot; deltas arriving before any
 * keyframe are skipped (a snapshot must begin with decodable state) without invalidating
 * it. Returns false only when this call voided the snapshot (byte or entry overflow). */
bool native_au_snapshot_append(NativeAuSnapshot *snap, const uint8_t *data, size_t len, bool is_keyframe,
                               uint64_t pts90k);

/* True when a replay would rebuild decodable state: armed and seeded with a keyframe. */
bool native_au_snapshot_ready(const NativeAuSnapshot *snap);

#endif
