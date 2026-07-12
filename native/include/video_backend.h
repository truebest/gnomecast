#ifndef GNOMECAST_VIDEO_BACKEND_H
#define GNOMECAST_VIDEO_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h264_annexb.h"
#include "media_backend.h"

typedef struct NativeVideo NativeVideo;

typedef enum NativeVideoResult {
    NATIVE_VIDEO_OK = 0,
    NATIVE_VIDEO_NEED_KEYFRAME = 1,
    NATIVE_VIDEO_UNSUPPORTED = 2,
    NATIVE_VIDEO_ERROR = 3,
    /* The AU was DISCARDED (decoder still loading), not decoded: benign transiently,
     * but callers must not count it as playback progress — a snapshot replay whose
     * AUs all land here has rebuilt nothing and must not be declared successful. */
    NATIVE_VIDEO_DROPPED = 4
} NativeVideoResult;

#define NATIVE_VIDEO_MAX_AU_BYTES NATIVE_H264_MAX_AU_BYTES

/* Attaches an H.264 video track to the shared media pipeline. The NativeMedia (and
 * the viewport, which lives on it) is owned by the caller and must outlive the
 * track. */
NativeVideo *native_video_open(NativeMedia *media, uint16_t width, uint16_t height, uint16_t fps);
/* Closes only the video track object; the shared NativeMedia stays alive. On webOS
 * the media adapter reloads any surviving audio track as part of the operation. */
void native_video_close(NativeVideo *video);
uint16_t native_video_width(const NativeVideo *video);
uint16_t native_video_height(const NativeVideo *video);
/* Feed one AVC length-prefixed or Annex-B H.264 access unit from RDPEGFX. The
 * implementation normalizes it to Annex-B before passing it to the backend.
 */
NativeVideoResult native_video_feed(NativeVideo *video, const uint8_t *data, size_t len, bool is_keyframe, uint64_t pts90k);

#endif
