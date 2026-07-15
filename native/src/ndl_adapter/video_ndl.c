#include "video_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HELLOLG_WITH_NDL
#include "media_ndl_internal.h"
#endif

#include "clog.h"

clog_define(g_native_log_video, cLogLevelInfo, cLogFlags_Default, "video.ndl", NULL);

struct NativeVideo {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint8_t *annexb;
    size_t annexb_cap;
    bool terminal_error;
#ifdef HELLOLG_WITH_NDL
    /* Borrowed from the caller; the track never closes either object. */
    NativeMedia *media;
    BackendNdl *backend;
    bool video_opened;
    bool feed_not_ready_logged;
    /* Feed diagnostics: with these in the log, "frames are being fed but the pipeline
     * never reported PLAYING" (e.g. a server resolution the TV cannot start on) is
     * visible at a glance next to the NDL load-callback lines. */
    uint32_t fed_au_count;
    uint32_t fed_keyframe_count;
#endif
};

#ifdef HELLOLG_WITH_NDL
/* NDL exposes no capability query (limits live inside the firmware); the stream is
 * opened lazily on the first config IDR, exactly like the pipeline load itself. */
static NativeVideoResult native_video_open_stream(NativeVideo *video) {
    BackendNdlVideoInfo info = {
        .codec = BACKEND_NDL_VIDEO_H264,
        .width = video->width,
        .height = video->height,
    };
    /* fps is bookkeeping only: the NDL load info carries no frame rate. */

    switch (native_media_ndl_configure_video(video->media, &info)) {
        case BACKEND_NDL_OK:
            video->video_opened = true;
            return NATIVE_VIDEO_OK;
        case BACKEND_NDL_UNSUPPORTED:
            video->terminal_error = true;
            clog(cLogLevelError, "NDL rejected H.264 stream as unsupported");
            return NATIVE_VIDEO_UNSUPPORTED;
        default:
            video->terminal_error = true;
            clog(cLogLevelError, "NDL failed to open H.264 stream");
            return NATIVE_VIDEO_ERROR;
    }
}
#endif

static bool native_video_reserve_annexb(NativeVideo *video, size_t needed) {
    if (needed <= video->annexb_cap) {
        return true;
    }
    uint8_t *next = (uint8_t *)realloc(video->annexb, needed);
    if (!next) {
        return false;
    }
    video->annexb = next;
    video->annexb_cap = needed;
    return true;
}

static void native_video_log_invalid_h264(const uint8_t *data, size_t len) {
    char head[16u * 3u + 5u] = "";
    size_t offset = 0;
    size_t preview = len < 16 ? len : 16;
    for (size_t i = 0; i < preview; i++) {
        int written = snprintf(head + offset, sizeof(head) - offset, "%s%02x", i == 0 ? "" : " ", data[i]);
        if (written < 0 || (size_t)written >= sizeof(head) - offset) {
            break;
        }
        offset += (size_t)written;
    }
    if (len > preview) {
        (void)snprintf(head + offset, sizeof(head) - offset, " ...");
    }
    clog(cLogLevelError, "unsupported H.264 access unit framing len=%zu first=%s", len, head);
}

NativeVideo *native_video_open(NativeMedia *media, uint16_t width, uint16_t height, uint16_t fps) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    (void)width;
    (void)height;
    (void)fps;
    clog(cLogLevelError, "NDL backend is not linked; native decoder unavailable");
    return NULL;
#else
    if (width == 0 || height == 0) {
        clog(cLogLevelError, "invalid video size %ux%u", (unsigned)width, (unsigned)height);
        return NULL;
    }
    BackendNdl *backend = native_media_ndl_backend(media);
    if (!backend) {
        clog(cLogLevelError, "no shared media backend available");
        return NULL;
    }

    NativeVideo *video = (NativeVideo *)calloc(1, sizeof(NativeVideo));
    if (!video) {
        return NULL;
    }
    video->width = width;
    video->height = height;
    video->fps = fps ? fps : 60;
    video->media = media;
    video->backend = backend;
    return video;
#endif
}

uint16_t native_video_width(const NativeVideo *video) {
    return video ? video->width : 0;
}

uint16_t native_video_height(const NativeVideo *video) {
    return video ? video->height : 0;
}

void native_video_close(NativeVideo *video) {
    if (!video) {
        return;
    }
#ifdef HELLOLG_WITH_NDL
    /* The media adapter reloads the surviving audio track atomically. */
    if (video->media && video->backend && video->video_opened) {
        (void)native_media_ndl_clear_video(video->media);
    }
#endif
    free(video->annexb);
    free(video);
}

NativeVideoResult native_video_feed(NativeVideo *video, const uint8_t *data, size_t len, bool is_keyframe, uint64_t pts90k) {
    (void)pts90k;
    if (!video || !data || len == 0) {
        return NATIVE_VIDEO_ERROR;
    }
    if (video->terminal_error) {
        return NATIVE_VIDEO_ERROR;
    }
    if (len > NATIVE_VIDEO_MAX_AU_BYTES) {
        clog(cLogLevelError, "AVC access unit %zu bytes exceeds %zu byte cap", len,
             (size_t)NATIVE_VIDEO_MAX_AU_BYTES);
        video->terminal_error = true;
        return NATIVE_VIDEO_ERROR;
    }

    NativeH264Info info;
    size_t annexb_len = 0;
    NativeH264Result avc_result = native_h264_avc_annexb_size(data, len, &info, &annexb_len);
    if (avc_result == NATIVE_H264_OK) {
        if (annexb_len > NATIVE_VIDEO_MAX_AU_BYTES) {
            clog(cLogLevelError, "Annex-B access unit %zu bytes exceeds %zu byte cap", annexb_len,
                 (size_t)NATIVE_VIDEO_MAX_AU_BYTES);
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
        }
        if (!native_video_reserve_annexb(video, annexb_len)) {
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
        }
        if (native_h264_avc_to_annexb(data, len, video->annexb, video->annexb_cap, &annexb_len) != NATIVE_H264_OK) {
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
        }
    } else if (native_h264_scan_annexb(data, len, &info) == NATIVE_H264_OK) {
        annexb_len = len;
        if (!native_video_reserve_annexb(video, annexb_len)) {
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
        }
        memcpy(video->annexb, data, annexb_len);
    } else {
        native_video_log_invalid_h264(data, len);
        video->terminal_error = true;
        return NATIVE_VIDEO_ERROR;
    }

#ifndef HELLOLG_WITH_NDL
    (void)is_keyframe;
    (void)info;
    (void)annexb_len;
    return NATIVE_VIDEO_UNSUPPORTED;
#else
    bool config_idr = info.has_sps && info.has_pps && info.has_idr;
    /* Only a real IDR clears the backend's after-reload keyframe gate. The transport
     * flag (and grd) deliberately mark parameter-set-only AUs as keyframes, but a
     * fresh decoder cannot start on an SPS without its IDR; clearing the gate on one
     * would let following P-frames report OK with no recovery signal. A gated
     * SPS-only AU is dropped as NEED_KEYFRAME; grd resends SPS+PPS with the IDR. */
    (void)is_keyframe;
    bool backend_keyframe = info.has_idr;

    if (!video->video_opened) {
        if (!config_idr) {
            return NATIVE_VIDEO_NEED_KEYFRAME;
        }
        NativeVideoResult open_result = native_video_open_stream(video);
        if (open_result != NATIVE_VIDEO_OK) {
            return open_result;
        }
    }

    BackendNdlResult feed_result = backend_ndl_feed_video(video->backend, video->annexb, annexb_len,
                                                          BACKEND_NDL_PTS_AUTO, backend_keyframe);

    switch (feed_result) {
        case BACKEND_NDL_OK:
            if (backend_keyframe) {
                video->fed_keyframe_count++;
            }
            video->feed_not_ready_logged = false;
            video->fed_au_count++;
            if (video->fed_au_count == 1 || video->fed_au_count == 60 || video->fed_au_count == 300 ||
                video->fed_au_count % 3600 == 0) {
                clog(cLogLevelDebug, "fed %u AUs (%u keyframes)", (unsigned)video->fed_au_count,
                     (unsigned)video->fed_keyframe_count);
            }
            return NATIVE_VIDEO_OK;
        case BACKEND_NDL_NEED_KEYFRAME:
            /* The pipeline was reloaded underneath (audio (re)open); the caller
             * requests a fresh server IDR and keeps deltas away until then. No
             * repeated-request escalation: the caller's keyframe watchdog owns
             * giving up on refresh-deaf servers. */
            return NATIVE_VIDEO_NEED_KEYFRAME;
        case BACKEND_NDL_NOT_READY:
            if (!video->feed_not_ready_logged) {
                clog(cLogLevelWarning, "NDL video feed not ready; dropping access units until decoder is ready");
                video->feed_not_ready_logged = true;
            }
            return NATIVE_VIDEO_DROPPED;
        default:
            clog(cLogLevelError, "NDL video feed returned %s", backend_ndl_result_name(feed_result));
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
    }
#endif
}
