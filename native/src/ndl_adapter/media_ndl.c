#include "media_backend.h"
#include "media_ndl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clog.h"

clog_define(g_native_log_media, cLogLevelInfo, cLogFlags_Default, "media.ndl", NULL);

#define GNOMECAST_NDL_FALLBACK_APP_ID "com.truebest.gnomecast.native"

struct NativeMedia {
    uint16_t viewport_width;
    uint16_t viewport_height;
#ifdef HELLOLG_WITH_NDL
    BackendNdl *backend;
    bool has_video;
    BackendNdlVideoInfo video;
    bool has_audio;
    BackendNdlPcmInfo audio;
#else
    int unused;
#endif
};

#ifdef HELLOLG_WITH_NDL
static void native_media_ndl_log(void *userdata, BackendNdlLogLevel level, const char *message) {
    (void)userdata;
    cLogLevel mapped = cLogLevelInfo;
    switch (level) {
        case BACKEND_NDL_LOG_DEBUG:
            mapped = cLogLevelDebug;
            break;
        case BACKEND_NDL_LOG_INFO:
            mapped = cLogLevelInfo;
            break;
        case BACKEND_NDL_LOG_WARN:
            mapped = cLogLevelWarning;
            break;
        default:
            mapped = cLogLevelError;
            break;
    }
    clog(mapped, "%s", message ? message : "");
}

/* backend_ndl gates its debug telemetry (NDL buffer polls, message formatting) on
 * minimum_log_level, so mirror the effective media.ndl level instead of formatting
 * everything for clog to discard. */
static BackendNdlLogLevel native_media_ndl_min_level(void) {
    if (clog_is_enabled(cLogLevelDebug)) {
        return BACKEND_NDL_LOG_DEBUG;
    }
    if (clog_is_enabled(cLogLevelInfo)) {
        return BACKEND_NDL_LOG_INFO;
    }
    if (clog_is_enabled(cLogLevelWarning)) {
        return BACKEND_NDL_LOG_WARN;
    }
    if (clog_is_enabled(cLogLevelError)) {
        return BACKEND_NDL_LOG_ERROR;
    }
    return BACKEND_NDL_LOG_OFF;
}

#ifdef HELLOLG_NDL_ADAPTER_TESTING
BackendNdlLogLevel native_media_ndl_test_min_level(void) {
    return native_media_ndl_min_level();
}
#endif

static BackendNdlResult native_media_ndl_apply_tracks(NativeMedia *media) {
    if (!media || !media->backend) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    return backend_ndl_set_media(media->backend, media->has_video ? &media->video : NULL,
                                 media->has_audio ? &media->audio : NULL);
}
#endif

NativeMedia *native_media_open(uint16_t viewport_width, uint16_t viewport_height) {
#ifndef HELLOLG_WITH_NDL
    (void)viewport_width;
    (void)viewport_height;
    clog(cLogLevelError, "NDL backend is not linked; hardware media unavailable");
    return NULL;
#else
    NativeMedia *media = (NativeMedia *)calloc(1, sizeof(NativeMedia));
    if (!media) {
        return NULL;
    }
    media->viewport_width = viewport_width;
    media->viewport_height = viewport_height;

    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    const char *app_id = getenv("APPID");
    config.app_id = app_id && app_id[0] ? app_id : GNOMECAST_NDL_FALLBACK_APP_ID;
    config.log_fn = native_media_ndl_log;
    config.minimum_log_level = native_media_ndl_min_level();
    config.require_keyframe_after_reload = true;
    config.guard_audio_overflow = true;
    config.prime_pcm_after_load = true;
    media->backend = backend_ndl_open(&config);
    if (!media->backend) {
        clog(cLogLevelError, "NDL DirectMedia backend unavailable");
        free(media);
        return NULL;
    }
    clog(cLogLevelNotice, "NDL DirectMedia backend ready (viewport %ux%u)",
         (unsigned)viewport_width, (unsigned)viewport_height);
    return media;
#endif
}

void native_media_close(NativeMedia *media) {
    if (!media) {
        return;
    }
#ifdef HELLOLG_WITH_NDL
    backend_ndl_close(media->backend);
#endif
    free(media);
}

void native_media_set_viewport(NativeMedia *media, uint16_t viewport_width, uint16_t viewport_height) {
    if (!media || viewport_width == 0 || viewport_height == 0) {
        return;
    }
    if (media->viewport_width == viewport_width && media->viewport_height == viewport_height) {
        return;
    }
    media->viewport_width = viewport_width;
    media->viewport_height = viewport_height;
#ifdef HELLOLG_WITH_NDL
    /* No hardware call: the NDL video plane scales to the panel on its own, and the
     * transparent SDL window above it handles UI-space scaling. Logged so viewport
     * churn stays visible next to reload events. */
    clog(cLogLevelDebug, "viewport=%ux%u", (unsigned)viewport_width, (unsigned)viewport_height);
#endif
}

BackendNdl *native_media_ndl_backend(NativeMedia *media) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    return NULL;
#else
    return media ? media->backend : NULL;
#endif
}

BackendNdlResult native_media_ndl_configure_video(NativeMedia *media,
                                                   const BackendNdlVideoInfo *info) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    (void)info;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !info) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_video = media->has_video;
    BackendNdlVideoInfo saved = media->video;
    media->has_video = true;
    media->video = *info;
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_video = had_video;
        media->video = saved;
    }
    return result;
#endif
}

BackendNdlResult native_media_ndl_configure_audio(NativeMedia *media,
                                                   const BackendNdlPcmInfo *info) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    (void)info;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !info) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_audio = media->has_audio;
    BackendNdlPcmInfo saved = media->audio;
    media->has_audio = true;
    media->audio = *info;
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_audio = had_audio;
        media->audio = saved;
    }
    return result;
#endif
}

BackendNdlResult native_media_ndl_clear_video(NativeMedia *media) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !media->backend) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_video = media->has_video;
    BackendNdlVideoInfo saved = media->video;
    media->has_video = false;
    memset(&media->video, 0, sizeof(media->video));
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_video = had_video;
        media->video = saved;
    }
    return result;
#endif
}

BackendNdlResult native_media_ndl_clear_audio(NativeMedia *media) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    return BACKEND_NDL_UNAVAILABLE;
#else
    if (!media || !media->backend) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    bool had_audio = media->has_audio;
    BackendNdlPcmInfo saved = media->audio;
    media->has_audio = false;
    memset(&media->audio, 0, sizeof(media->audio));
    BackendNdlResult result = native_media_ndl_apply_tracks(media);
    if (result != BACKEND_NDL_OK) {
        media->has_audio = had_audio;
        media->audio = saved;
    }
    return result;
#endif
}
