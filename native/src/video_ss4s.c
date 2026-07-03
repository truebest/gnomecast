#include "video_ss4s.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HELLOLG_WITH_SS4S
#include <stdarg.h>

#include <ss4s.h>
#endif

struct NativeVideo {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint8_t *annexb;
    size_t annexb_cap;
    bool terminal_error;
#ifdef HELLOLG_WITH_SS4S
    /* Borrowed from the caller's NativeMedia; the track never closes the player. */
    SS4S_Player *player;
    bool video_opened;
    bool decoder_keyframe_pending;
    bool feed_not_ready_logged;
#endif
};

#ifdef HELLOLG_WITH_SS4S
/* Valid any time after SS4S_PostInit (i.e. once native_media_open succeeded). */
static bool native_video_check_capabilities(const NativeVideo *video) {
    SS4S_VideoCapabilities capabilities;
    memset(&capabilities, 0, sizeof(capabilities));
    if (!SS4S_GetVideoCapabilities(&capabilities) || (capabilities.codecs & SS4S_VIDEO_H264) == 0) {
        fprintf(stderr, "[native-video] ss4s module does not report H.264 support\n");
        return false;
    }
    if ((capabilities.maxWidth != 0 && video->width > capabilities.maxWidth) ||
        (capabilities.maxHeight != 0 && video->height > capabilities.maxHeight) ||
        (capabilities.maxFps != 0 && video->fps > capabilities.maxFps)) {
        fprintf(stderr, "[native-video] requested %ux%u@%u exceeds ss4s module capabilities\n",
                (unsigned)video->width, (unsigned)video->height, (unsigned)video->fps);
        return false;
    }
    return true;
}

static NativeVideoResult native_video_open_stream(NativeVideo *video) {
    SS4S_VideoInfo info;
    memset(&info, 0, sizeof(info));
    info.codec = SS4S_VIDEO_H264;
    info.width = video->width;
    info.height = video->height;
    info.frameRateNumerator = video->fps ? video->fps : 60;
    info.frameRateDenominator = 1;

    switch (SS4S_PlayerVideoOpen(video->player, &info)) {
        case SS4S_VIDEO_OPEN_OK:
            video->video_opened = true;
            return NATIVE_VIDEO_OK;
        case SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC:
            video->terminal_error = true;
            fprintf(stderr, "[native-video] ss4s rejected H.264 stream as unsupported\n");
            return NATIVE_VIDEO_UNSUPPORTED;
        case SS4S_VIDEO_OPEN_ERROR:
        default:
            video->terminal_error = true;
            fprintf(stderr, "[native-video] ss4s failed to open H.264 stream\n");
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
    fprintf(stderr, "[native-video] unsupported H.264 access unit framing len=%zu first=", len);
    size_t preview = len < 16 ? len : 16;
    for (size_t i = 0; i < preview; i++) {
        fprintf(stderr, "%02x", data[i]);
        if (i + 1 < preview) {
            fputc(' ', stderr);
        }
    }
    if (len > preview) {
        fprintf(stderr, " ...");
    }
    fputc('\n', stderr);
}

NativeVideo *native_video_open(NativeMedia *media, uint16_t width, uint16_t height, uint16_t fps) {
#ifndef HELLOLG_WITH_SS4S
    (void)media;
    (void)width;
    (void)height;
    (void)fps;
    fprintf(stderr, "[native-video] ss4s is not linked; native decoder unavailable\n");
    return NULL;
#else
    if (width == 0 || height == 0) {
        fprintf(stderr, "[native-video] invalid video size %ux%u\n", (unsigned)width, (unsigned)height);
        return NULL;
    }
    SS4S_Player *player = native_media_player(media);
    if (!player) {
        fprintf(stderr, "[native-video] no shared media player available\n");
        return NULL;
    }

    NativeVideo *video = (NativeVideo *)calloc(1, sizeof(NativeVideo));
    if (!video) {
        return NULL;
    }
    video->width = width;
    video->height = height;
    video->fps = fps ? fps : 60;
    video->player = player;

    if (!native_video_check_capabilities(video)) {
        native_video_close(video);
        return NULL;
    }
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
#ifdef HELLOLG_WITH_SS4S
    /* The shared player belongs to the caller's NativeMedia and is not closed here.
     * NOTE: on the webOS backends SS4S_PlayerVideoClose unloads the shared media
     * pipeline — it is NOT track-only. Callers with a live audio track must reopen it
     * afterwards (cheap: Opus/PCM need no keyframe), and a subsequent video open
     * triggers a fresh pipeline load anyway. */
    if (video->player && video->video_opened) {
        (void)SS4S_PlayerVideoClose(video->player);
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
        fprintf(stderr, "[native-video] AVC access unit %zu bytes exceeds %zu byte cap\n", len,
                (size_t)NATIVE_VIDEO_MAX_AU_BYTES);
        video->terminal_error = true;
        return NATIVE_VIDEO_ERROR;
    }

    NativeH264Info info;
    size_t annexb_len = 0;
    NativeH264Result avc_result = native_h264_avc_annexb_size(data, len, &info, &annexb_len);
    if (avc_result == NATIVE_H264_OK) {
        if (annexb_len > NATIVE_VIDEO_MAX_AU_BYTES) {
            fprintf(stderr, "[native-video] Annex-B access unit %zu bytes exceeds %zu byte cap\n", annexb_len,
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

#ifndef HELLOLG_WITH_SS4S
    (void)is_keyframe;
    (void)info;
    (void)annexb_len;
    return NATIVE_VIDEO_UNSUPPORTED;
#else
    bool config_idr = info.has_sps && info.has_pps && info.has_idr;
    bool ss4s_keyframe = is_keyframe || info.has_idr || info.has_sps;

    if (!video->video_opened) {
        if (!config_idr) {
            return NATIVE_VIDEO_NEED_KEYFRAME;
        }
        NativeVideoResult open_result = native_video_open_stream(video);
        if (open_result != NATIVE_VIDEO_OK) {
            return open_result;
        }
    }

    if (video->decoder_keyframe_pending && !ss4s_keyframe) {
        return NATIVE_VIDEO_NEED_KEYFRAME;
    }

    SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
    if (ss4s_keyframe) {
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
    }

    SS4S_VideoFeedResult feed_result = SS4S_PlayerVideoFeed(video->player, video->annexb, annexb_len, flags);

    switch (feed_result) {
        case SS4S_VIDEO_FEED_OK:
            if (ss4s_keyframe) {
                video->decoder_keyframe_pending = false;
            }
            video->feed_not_ready_logged = false;
            return NATIVE_VIDEO_OK;
        case SS4S_VIDEO_FEED_REQUEST_KEYFRAME:
            if (video->decoder_keyframe_pending) {
                fprintf(stderr, "[native-video] ss4s repeated keyframe request after recovery keyframe\n");
                video->terminal_error = true;
                return NATIVE_VIDEO_ERROR;
            }
            video->decoder_keyframe_pending = true;
            return NATIVE_VIDEO_NEED_KEYFRAME;
        case SS4S_VIDEO_FEED_NOT_READY:
            if (!video->feed_not_ready_logged) {
                fprintf(stderr, "[native-video] ss4s video feed not ready; dropping access units until decoder is ready\n");
                video->feed_not_ready_logged = true;
            }
            return NATIVE_VIDEO_OK;
        case SS4S_VIDEO_FEED_ERROR:
            fprintf(stderr, "[native-video] ss4s video feed returned error\n");
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
        default:
            fprintf(stderr, "[native-video] ss4s video feed returned unexpected result\n");
            video->terminal_error = true;
            return NATIVE_VIDEO_ERROR;
    }
#endif
}
