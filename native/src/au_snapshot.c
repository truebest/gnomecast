#include "au_snapshot.h"

#include <stdlib.h>
#include <string.h>

bool native_au_snapshot_arm(NativeAuSnapshot *snap) {
    if (!snap) {
        return false;
    }
    native_au_snapshot_reset(snap);
    snap->buf = (uint8_t *)malloc(NATIVE_AU_SNAPSHOT_MAX_BYTES);
    if (!snap->buf) {
        return false;
    }
    snap->armed = true;
    return true;
}

void native_au_snapshot_reset(NativeAuSnapshot *snap) {
    if (!snap) {
        return;
    }
    free(snap->buf);
    snap->buf = NULL;
    snap->used = 0;
    snap->count = 0;
    snap->armed = false;
    snap->has_idr = false;
}

bool native_au_snapshot_append(NativeAuSnapshot *snap, const uint8_t *data, size_t len, bool is_keyframe,
                               uint64_t pts90k) {
    if (!snap || !snap->armed) {
        return false;
    }
    if (!data || len == 0) {
        return true; /* nothing to cache; the snapshot stays valid */
    }
    if (is_keyframe) {
        /* Only the newest IDR group can seed a decoder; older content is obsolete. */
        snap->used = 0;
        snap->count = 0;
        snap->has_idr = true;
    } else if (!snap->has_idr) {
        return true; /* delta from before the first keyframe: undecodable alone, skip */
    }
    if (len > NATIVE_AU_SNAPSHOT_MAX_BYTES - snap->used || snap->count >= NATIVE_AU_SNAPSHOT_MAX_AUS) {
        /* A partial delta chain is undecodable, so overflow voids the whole snapshot. */
        native_au_snapshot_reset(snap);
        return false;
    }
    memcpy(snap->buf + snap->used, data, len);
    snap->entries[snap->count].offset = snap->used;
    snap->entries[snap->count].len = len;
    snap->entries[snap->count].is_keyframe = is_keyframe;
    snap->entries[snap->count].pts90k = pts90k;
    snap->used += len;
    snap->count++;
    return true;
}

bool native_au_snapshot_ready(const NativeAuSnapshot *snap) {
    return snap && snap->armed && snap->has_idr && snap->count > 0;
}
