#include "au_snapshot.h"

#include "h264_annexb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_video, cLogLevelInfo, cLogFlags_Default, "video.snapshot", NULL);

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

/* Scan of the AVC length-prefixed AU for an actual IDR slice (NAL type 5). The
 * transport's is_keyframe flag also fires on parameter-set-only AUs (a standalone SPS
 * is NAL type 7): seeding the cache from one of those would mark the snapshot ready
 * with nothing a decoder can start from — the switch tick would then suppress the
 * server against an undecodable cache.
 *
 * The verdict is deliberately three-way. The transport's keyframe detector returns at
 * the FIRST parameter-set/IDR NAL and never validates the rest of the AU's framing;
 * this scan must walk PAST the parameter sets to find the IDR, so a framing quirk in
 * the tail (padding, vendor prefixes) that the detector never sees would otherwise
 * silently veto every real keyframe — observed live as "snapshots never ready, every
 * switch degrades to a reconnect". Unparseable framing therefore falls back to
 * TRUSTING the transport flag; only a fully parsed AU with no IDR overrides it. */
typedef enum NativeAuIdrScan {
    NATIVE_AU_SCAN_NO_IDR = 0,
    NATIVE_AU_SCAN_HAS_IDR,
    NATIVE_AU_SCAN_UNPARSEABLE,
} NativeAuIdrScan;

static NativeAuIdrScan au_snapshot_scan_idr(const uint8_t *data, size_t len) {
    /* The SAME dual-framing detection the decoder adapter runs (ndl_adapter/video_ndl.c): the nominal
     * RDPEGFX shape is AVC length-prefixed, but gnome-remote-desktop delivers Annex-B.
     * Reusing the feed's parsers keeps the invariant "the snapshot seeds exactly from
     * what the decoder can play". */
    NativeH264Info info;
    if (native_h264_scan_avc(data, len, &info) == NATIVE_H264_OK ||
        native_h264_scan_annexb(data, len, &info) == NATIVE_H264_OK) {
        return info.has_idr ? NATIVE_AU_SCAN_HAS_IDR : NATIVE_AU_SCAN_NO_IDR;
    }
    return NATIVE_AU_SCAN_UNPARSEABLE;
}

bool native_au_snapshot_append(NativeAuSnapshot *snap, const uint8_t *data, size_t len, bool is_keyframe,
                               uint64_t pts90k) {
    if (!snap || !snap->armed) {
        return false;
    }
    if (!data || len == 0) {
        return true; /* nothing to cache; the snapshot stays valid */
    }
    NativeAuIdrScan scan = is_keyframe ? au_snapshot_scan_idr(data, len) : NATIVE_AU_SCAN_NO_IDR;
    if (is_keyframe && scan != NATIVE_AU_SCAN_NO_IDR) {
        if (scan == NATIVE_AU_SCAN_UNPARSEABLE) {
            /* Diagnostic breadcrumb (once): the framing quirk is worth knowing about,
             * but the transport flag is the authority for these AUs. */
            static bool logged = false;
            if (!logged) {
                logged = true;
                clog(cLogLevelWarning,
                     "keyframe AU framing unparseable (len=%zu, head %02x%02x%02x%02x%02x); trusting the transport keyframe flag",
                     len, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
                     len > 3 ? data[3] : 0, len > 4 ? data[4] : 0);
            }
        }
        /* Only the newest IDR group can seed a decoder; older content is obsolete. */
        snap->used = 0;
        snap->count = 0;
        snap->has_idr = true;
    } else if (!snap->has_idr) {
        /* Delta — or a PARSED keyframe-flagged AU with no IDR in it (a standalone
         * SPS) — from before the first real IDR: undecodable alone, skip. Once an IDR
         * seeded the cache, later config-only AUs append as ordinary chain members. */
        return true;
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
