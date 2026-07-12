/* SPDX-License-Identifier: MIT */
#ifndef BACKEND_NDL_BACKEND_NDL_H
#define BACKEND_NDL_BACKEND_NDL_H

/*
 * Small C11 wrapper around the process-global webOS NDL DirectMedia API.
 *
 * The public interface deliberately contains no webOS SDK types. The library
 * resolves libNDL_directmedia with dlopen, so applications can stay loadable on
 * systems where the firmware library is absent.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BackendNdl BackendNdl;

typedef enum BackendNdlResult {
    BACKEND_NDL_OK = 0,
    BACKEND_NDL_ERROR = 1,
    BACKEND_NDL_UNAVAILABLE = 2,
    BACKEND_NDL_UNSUPPORTED = 3,
    BACKEND_NDL_INVALID_ARGUMENT = 4,
    BACKEND_NDL_NOT_READY = 5,
    BACKEND_NDL_OVERFLOW = 6,
    BACKEND_NDL_NEED_KEYFRAME = 7
} BackendNdlResult;

typedef enum BackendNdlVideoCodec {
    BACKEND_NDL_VIDEO_NONE = 0,
    BACKEND_NDL_VIDEO_H264 = 1,
    BACKEND_NDL_VIDEO_H265 = 2,
    BACKEND_NDL_VIDEO_VP9 = 3,
    BACKEND_NDL_VIDEO_AV1 = 4
} BackendNdlVideoCodec;

typedef enum BackendNdlLogLevel {
    BACKEND_NDL_LOG_DEBUG = 0,
    BACKEND_NDL_LOG_INFO = 1,
    BACKEND_NDL_LOG_WARN = 2,
    BACKEND_NDL_LOG_ERROR = 3,
    BACKEND_NDL_LOG_OFF = 4
} BackendNdlLogLevel;

/* Callbacks are synchronous. They may originate from a firmware callback
 * thread and must remain valid until backend_ndl_close() returns. Do not call
 * back into this BackendNdl from a callback. */
typedef void (*BackendNdlLogFn)(void *userdata, BackendNdlLogLevel level, const char *message);
typedef void (*BackendNdlResourceReleasedFn)(void *userdata, const char *resource_type);
typedef void (*BackendNdlMediaEventFn)(void *userdata, int type, int64_t number, const char *text);

typedef struct BackendNdlConfig {
    /* Required non-empty webOS application id. The library has no application-specific fallback. */
    const char *app_id;

    /* Optional runtime library override. NULL tries known firmware sonames, then RTLD_DEFAULT. */
    const char *library_path;

    /* Optional callbacks and their shared context. A NULL callback disables that notification. */
    BackendNdlLogFn log_fn;
    BackendNdlResourceReleasedFn resource_released_fn;
    BackendNdlMediaEventFn media_event_fn;
    void *userdata;
    BackendNdlLogLevel minimum_log_level;

    /* Initialize the firmware's dynamic implementation when its optional entry points exist. */
    bool call_dl_initialize;

    /* Applied after every load. A negative value leaves the firmware default unchanged. */
    int video_frame_drop_threshold;

    /* Gate video after each load until the caller supplies a keyframe. */
    bool require_keyframe_after_reload;

    /* Check the optional NDL available-byte counter before feeding audio. */
    bool guard_audio_overflow;

    /* Feed one silent PCM frame after a load containing an audio track. */
    bool prime_pcm_after_load;
} BackendNdlConfig;

typedef struct BackendNdlVideoInfo {
    BackendNdlVideoCodec codec;
    uint32_t width;
    uint32_t height;
} BackendNdlVideoInfo;

typedef struct BackendNdlPcmInfo {
    /* Must map onto the firmware PCM sample-rate table. */
    uint32_t sample_rate_hz;
    /* 1 (mono) or 2 (stereo). */
    uint16_t channels;
} BackendNdlPcmInfo;

/* Generic policy-free defaults: callbacks/app_id/library_path are NULL,
 * minimum_log_level=INFO, dynamic initialization and overflow guarding are on,
 * frame-drop threshold=-1, keyframe gating and PCM priming are off. */
void backend_ndl_config_defaults(BackendNdlConfig *config);

/* Loads and initializes DirectMedia. NDL is process-global, so only one
 * context may exist at a time. config and config->app_id must be non-NULL;
 * app_id is copied. Returns NULL when unavailable or initialization fails.
 * Concurrent open/close, or close concurrent with another API call, is not supported. */
BackendNdl *backend_ndl_open(const BackendNdlConfig *config);
void backend_ndl_close(BackendNdl *ctx);

/* Atomically replace the complete media configuration. Either track pointer
 * may be NULL; both NULL unload and clear the pipeline. NDL loads both tracks
 * as one unit. On failure the previous configuration is restored best-effort. */
BackendNdlResult backend_ndl_set_media(BackendNdl *ctx,
                                      const BackendNdlVideoInfo *video,
                                      const BackendNdlPcmInfo *audio);
BackendNdlResult backend_ndl_unload(BackendNdl *ctx);

/* Explicit unload+load with the currently configured tracks. */
BackendNdlResult backend_ndl_reload(BackendNdl *ctx);

/* Feed one elementary-stream access unit. PTS is passed verbatim to NDL.
 * NOT_READY means the pipeline is not loaded; NEED_KEYFRAME means a load
 * re-armed the optional keyframe gate. */
BackendNdlResult backend_ndl_feed_video(BackendNdl *ctx,
                                        const void *data,
                                        size_t len,
                                        long long ndl_pts,
                                        bool is_keyframe);

/* Feed one interleaved S16LE PCM chunk. NOT_READY/OVERFLOW means it was dropped. */
BackendNdlResult backend_ndl_feed_audio(BackendNdl *ctx,
                                        const void *data,
                                        size_t len,
                                        long long ndl_pts);

/* Milliseconds of CLOCK_MONOTONIC elapsed since the last successful load; 0 before it. */
long long backend_ndl_media_time_ms(const BackendNdl *ctx);

/* Optional-symbol passthroughs; UNAVAILABLE means the firmware lacks the symbol. */
BackendNdlResult backend_ndl_get_audio_available(BackendNdl *ctx, int *available_bytes);
BackendNdlResult backend_ndl_get_audio_total(BackendNdl *ctx, int *total_bytes);
BackendNdlResult backend_ndl_get_video_render_buffer_length(BackendNdl *ctx, int *frames);
BackendNdlResult backend_ndl_flush_video_render_buffer(BackendNdl *ctx);
BackendNdlResult backend_ndl_set_video_area(BackendNdl *ctx, int left, int top, int width, int height);
BackendNdlResult backend_ndl_set_foreground(BackendNdl *ctx, bool foreground);

bool backend_ndl_pcm_sample_rate_supported(uint32_t hz);
const char *backend_ndl_result_name(BackendNdlResult result);
const char *backend_ndl_log_level_name(BackendNdlLogLevel level);

#ifdef __cplusplus
}
#endif

#endif
