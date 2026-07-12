/* SPDX-License-Identifier: MIT */
/* RTLD_DEFAULT needs _GNU_SOURCE on glibc; must precede every include. */
#define _GNU_SOURCE

#include "backend_ndl_api.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(BACKEND_NDL_ENABLED) && BACKEND_NDL_ENABLED
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#endif

#define BACKEND_NDL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

void backend_ndl_config_defaults(BackendNdlConfig *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->minimum_log_level = BACKEND_NDL_LOG_INFO;
    config->call_dl_initialize = true;
    config->guard_audio_overflow = true;
    config->video_frame_drop_threshold = -1;
}

const char *backend_ndl_result_name(BackendNdlResult result) {
    switch (result) {
        case BACKEND_NDL_OK:
            return "OK";
        case BACKEND_NDL_ERROR:
            return "ERROR";
        case BACKEND_NDL_UNAVAILABLE:
            return "UNAVAILABLE";
        case BACKEND_NDL_UNSUPPORTED:
            return "UNSUPPORTED";
        case BACKEND_NDL_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case BACKEND_NDL_NOT_READY:
            return "NOT_READY";
        case BACKEND_NDL_OVERFLOW:
            return "OVERFLOW";
        case BACKEND_NDL_NEED_KEYFRAME:
            return "NEED_KEYFRAME";
        default:
            return "UNKNOWN";
    }
}

const char *backend_ndl_log_level_name(BackendNdlLogLevel level) {
    switch (level) {
        case BACKEND_NDL_LOG_DEBUG:
            return "DEBUG";
        case BACKEND_NDL_LOG_INFO:
            return "INFO";
        case BACKEND_NDL_LOG_WARN:
            return "WARN";
        case BACKEND_NDL_LOG_ERROR:
            return "ERROR";
        case BACKEND_NDL_LOG_OFF:
            return "OFF";
        default:
            return "UNKNOWN";
    }
}

static void backend_ndl_vlog_config(const BackendNdlConfig *config,
                                    BackendNdlLogLevel level,
                                    const char *format,
                                    va_list args) {
    if (!config || !config->log_fn || !format || level < config->minimum_log_level ||
        config->minimum_log_level >= BACKEND_NDL_LOG_OFF) {
        return;
    }
    char message[1024];
    int written = vsnprintf(message, sizeof(message), format, args);
    if (written < 0) {
        (void)snprintf(message, sizeof(message), "<format-error>");
    } else if ((size_t)written >= sizeof(message)) {
        static const char marker[] = " <truncated>";
        memcpy(message + sizeof(message) - sizeof(marker), marker, sizeof(marker));
    }
    for (char *p = message; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    config->log_fn(config->userdata, level, message);
}

static void backend_ndl_log_config(const BackendNdlConfig *config,
                                   BackendNdlLogLevel level,
                                   const char *format,
                                   ...) {
    va_list args;
    va_start(args, format);
    backend_ndl_vlog_config(config, level, format, args);
    va_end(args);
}

#if defined(BACKEND_NDL_ENABLED) && BACKEND_NDL_ENABLED

struct BackendNdl {
    pthread_mutex_t lock;
    BackendNdlConfig config;

    char *app_id;

    void *dl_handle;
    bool dl_handle_is_default;
    BackendNdlApi api;

    bool ndl_dl_initialized;
    bool ndl_initialized;
    bool media_loaded;
    bool need_keyframe;
    struct timespec loaded_time;
    uint64_t reload_generation;

    bool has_video;
    BackendNdlVideoInfo video;
    bool has_audio;
    BackendNdlPcmInfo pcm;

    /* Feed telemetry: drop streaks make the wrapper's log-once episode markers
     * conclusive (a lone "overflow" line cannot distinguish one dropped block
     * from a sink that never recovered; the recovery summary can). */
    uint32_t audio_overflow_streak;
    uint32_t audio_not_ready_streak;
    uint64_t audio_feed_count;
    uint64_t video_feed_count;
};

/* NDL DirectMedia is process-global state (Load/Unload take no handle), so the
 * context is a singleton. Also gives the static NDL callbacks an owner.
 *
 * The registry lock makes teardown safe against the firmware's callback
 * threads: callbacks hold it for their whole (cheap) body, and close() clears
 * the registration under it BEFORE tearing the context down — acquiring the
 * lock there waits out any callback already running, and later callbacks see
 * no owner. Without this, the UNLOADCOMPLETED event triggered by close()'s own
 * unload could dereference the context after free. */
static pthread_mutex_t g_backend_ndl_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static BackendNdl *g_backend_ndl_active = NULL;

static void backend_ndl_log(BackendNdl *ctx, BackendNdlLogLevel level, const char *format, ...) {
    if (!ctx) {
        return;
    }
    va_list args;
    va_start(args, format);
    backend_ndl_vlog_config(&ctx->config, level, format, args);
    va_end(args);
}

static bool backend_ndl_log_enabled(const BackendNdl *ctx, BackendNdlLogLevel level) {
    return ctx && ctx->config.log_fn && level >= ctx->config.minimum_log_level &&
           ctx->config.minimum_log_level < BACKEND_NDL_LOG_OFF;
}

#define BACKEND_NDL_LOG(ctx_, level_, ...) backend_ndl_log((ctx_), (level_), __VA_ARGS__)
#define BACKEND_NDL_DEBUG_LOG(ctx_, ...) \
    BACKEND_NDL_LOG((ctx_), BACKEND_NDL_LOG_DEBUG, __VA_ARGS__)

/* Sample high-frequency audio feed telemetry occasionally. */
#define BACKEND_NDL_AUDIO_DEBUG_CADENCE 512
/* Log the video render queue depth at the same order as the AU-count logs. */
#define BACKEND_NDL_VIDEO_DEBUG_CADENCE 300

static char *backend_ndl_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

static const char *backend_ndl_last_error_locked(const BackendNdl *ctx) {
    if (!ctx || !ctx->api.DirectMediaGetError) {
        return "(NDL error unavailable)";
    }
    const char *err = ctx->api.DirectMediaGetError();
    return err ? err : "(NDL returned no error text)";
}

static void *backend_ndl_sym(BackendNdl *ctx, void *handle, const char *name, bool required) {
    dlerror();
    void *sym = dlsym(handle, name);
    const char *err = dlerror();
    if (!sym && required) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR, "missing required symbol %s: %s", name,
                        err ? err : "not found");
    }
    return sym;
}

static bool backend_ndl_resolve_api(BackendNdl *ctx) {
#define REQ(name) do { \
        ctx->api.name = (__typeof__(ctx->api.name))backend_ndl_sym(ctx, ctx->dl_handle, "NDL_" #name, true); \
        if (!ctx->api.name) { return false; } \
    } while (0)

#define OPT(name) do { \
        ctx->api.name = (__typeof__(ctx->api.name))backend_ndl_sym(ctx, ctx->dl_handle, "NDL_" #name, false); \
        if (!ctx->api.name) { \
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_DEBUG, "optional symbol NDL_" #name " not present"); \
        } \
    } while (0)

    REQ(DirectMediaGetError);
    REQ(DirectMediaInit);
    REQ(DirectMediaQuit);
    REQ(DirectMediaLoad);
    REQ(DirectMediaUnload);
    REQ(DirectVideoPlay);
    REQ(DirectAudioPlay);

    OPT(DirectMedia_DL_Initialize);
    OPT(DirectMedia_DL_Finalize);
    OPT(DirectMedia_DL_IsInitialized);
    OPT(DirectMediaSetAppState);
    OPT(DirectVideoSetArea);
    OPT(DirectVideoSetFrameDropThreshold);
    OPT(DirectVideoFlushRenderBuffer);
    OPT(DirectVideoGetRenderBufferLength);
    OPT(DirectAudioGetAvailableBufferSize);
    OPT(DirectAudioGetTotalBufferSize);

#undef REQ
#undef OPT
    return true;
}

static bool backend_ndl_open_library(BackendNdl *ctx) {
    /* Sonames actually shipped by webOS firmware / the NDK sysroot. */
    static const char *const candidates[] = {
        "libNDL_directmedia.so.1",
        "libNDL_directmedia.so.1.0.0",
        "libNDL_directmedia.so",
    };

    const char *override = ctx->config.library_path;
    if (override && override[0] != '\0') {
        ctx->dl_handle = dlopen(override, RTLD_NOW | RTLD_LOCAL);
        if (!ctx->dl_handle) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR, "dlopen(%s) failed: %s", override, dlerror());
            return false;
        }
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "loaded NDL library: %s", override);
        return true;
    }

    for (size_t i = 0; i < BACKEND_NDL_ARRAY_LEN(candidates); i++) {
        ctx->dl_handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (ctx->dl_handle) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "loaded NDL library: %s", candidates[i]);
            return true;
        }
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_DEBUG, "dlopen(%s) failed: %s", candidates[i], dlerror());
    }

    /*
     * Last chance: symbols may already be present because something linked
     * against the library directly. RTLD_DEFAULT is not dlclose-able.
     */
    ctx->dl_handle = RTLD_DEFAULT;
    ctx->dl_handle_is_default = true;
    BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_DEBUG, "falling back to RTLD_DEFAULT for NDL symbols");
    return true;
}

static void backend_ndl_load_callback(int type, long long num_value, const char *str_value) {
    /* Runs on a firmware thread; see g_backend_ndl_registry_lock. Only the
     * immutable config is touched here — never ctx->lock, never NDL. */
    pthread_mutex_lock(&g_backend_ndl_registry_lock);
    BackendNdl *ctx = g_backend_ndl_active;
    /* Diagnostics only; media_loaded is set synchronously after Load returns.
     * Known STATE_UPDATE codes observed on webOS 5+ firmware. */
    switch (type) {
        case 0x16:
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "load callback: LOADCOMPLETED %s",
                            str_value ? str_value : "");
            break;
        case 0x17:
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "load callback: UNLOADCOMPLETED %s",
                            str_value ? str_value : "");
            break;
        case 0x1a:
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "load callback: PLAYING %s",
                            str_value ? str_value : "");
            break;
        default:
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_DEBUG, "load callback: type=0x%x num=%lld text=%s",
                            type, num_value, str_value ? str_value : "(null)");
            break;
    }
    if (ctx && ctx->config.media_event_fn) {
        ctx->config.media_event_fn(ctx->config.userdata, type, (int64_t)num_value, str_value);
    }
    pthread_mutex_unlock(&g_backend_ndl_registry_lock);
}

static void backend_ndl_resource_released_callback(const char *type) {
    /* Runs on a firmware thread; see g_backend_ndl_registry_lock. */
    pthread_mutex_lock(&g_backend_ndl_registry_lock);
    BackendNdl *ctx = g_backend_ndl_active;
    BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_WARN, "resource released by firmware: type=%s",
                    type ? type : "(null)");
    if (ctx && ctx->config.resource_released_fn) {
        ctx->config.resource_released_fn(ctx->config.userdata, type);
    }
    pthread_mutex_unlock(&g_backend_ndl_registry_lock);
}

static long long backend_ndl_media_time_ms_locked(const BackendNdl *ctx) {
    if (ctx->loaded_time.tv_sec == 0 && ctx->loaded_time.tv_nsec == 0) {
        return 0;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)(now.tv_sec - ctx->loaded_time.tv_sec) * 1000 +
           (now.tv_nsec - ctx->loaded_time.tv_nsec) / 1000000;
}

static const char *backend_ndl_pcm_channel_mode(uint16_t channels) {
    switch (channels) {
        case 1:
            /* The sysroot header defines no mono macro; the firmware matches
             * these strings ("mono"/"stereo"/"6-channel"). */
            return "mono";
        case 2:
            return NDL_DIRECTMEDIA_AUDIO_PCM_MODE_STEREO;
        default:
            return NULL;
    }
}

static BackendNdlResult backend_ndl_build_media_info_locked(BackendNdl *ctx,
                                                            NDL_DIRECTMEDIA_DATA_INFO_T *out) {
    if (!ctx->has_video && !ctx->has_audio) {
        return BACKEND_NDL_NOT_READY;
    }

    /* Zeroed = "track absent" (type 0) for both members. */
    memset(out, 0, sizeof(*out));

    if (ctx->has_video) {
        switch (ctx->video.codec) {
            case BACKEND_NDL_VIDEO_H264:
                out->video.type = NDL_VIDEO_TYPE_H264;
                break;
            case BACKEND_NDL_VIDEO_H265:
                out->video.type = NDL_VIDEO_TYPE_H265;
                break;
            case BACKEND_NDL_VIDEO_VP9:
                out->video.type = NDL_VIDEO_TYPE_VP9;
                break;
            case BACKEND_NDL_VIDEO_AV1:
                out->video.type = NDL_VIDEO_TYPE_AV1;
                break;
            case BACKEND_NDL_VIDEO_NONE:
            default:
                return BACKEND_NDL_UNSUPPORTED;
        }
        if (ctx->video.width == 0 || ctx->video.height == 0) {
            return BACKEND_NDL_INVALID_ARGUMENT;
        }
        out->video.width = (int)ctx->video.width;
        out->video.height = (int)ctx->video.height;
        out->video.unknown1 = 0;
    }

    if (ctx->has_audio) {
        const char *mode = backend_ndl_pcm_channel_mode(ctx->pcm.channels);
        if (!mode) {
            return BACKEND_NDL_UNSUPPORTED;
        }
        NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE sample_rate =
            NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF((int)ctx->pcm.sample_rate_hz);
        if (sample_rate == NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_NONE) {
            return BACKEND_NDL_UNSUPPORTED;
        }
        out->audio.pcm.type = NDL_AUDIO_TYPE_PCM;
        out->audio.pcm.unknown1 = 0;
        out->audio.pcm.format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE;
        /* layout stays NULL: the sysroot header defines no layout macro and the
         * firmware accepts NULL for interleaved S16LE. */
        out->audio.pcm.channelMode = mode;
        out->audio.pcm.sampleRate = sample_rate;
    }

    return BACKEND_NDL_OK;
}

static BackendNdlResult backend_ndl_unload_locked(BackendNdl *ctx) {
    if (!ctx->media_loaded) {
        return BACKEND_NDL_OK;
    }

    ctx->media_loaded = false;
    ctx->need_keyframe = false;
    int rc = ctx->api.DirectMediaUnload();
    if (rc != 0) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR, "NDL_DirectMediaUnload failed rc=%d error=%s", rc,
                        backend_ndl_last_error_locked(ctx));
        return BACKEND_NDL_ERROR;
    }
    BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "media unloaded");
    return BACKEND_NDL_OK;
}

static BackendNdlResult backend_ndl_reload_locked(BackendNdl *ctx) {
    if (!ctx->ndl_initialized) {
        return BACKEND_NDL_NOT_READY;
    }

    BackendNdlResult ur = backend_ndl_unload_locked(ctx);
    if (ur != BACKEND_NDL_OK) {
        return ur;
    }

    if (!ctx->has_video && !ctx->has_audio) {
        return BACKEND_NDL_OK;
    }

    NDL_DIRECTMEDIA_DATA_INFO_T info;
    BackendNdlResult br = backend_ndl_build_media_info_locked(ctx, &info);
    if (br != BACKEND_NDL_OK) {
        return br;
    }

    int rc = ctx->api.DirectMediaLoad(&info, backend_ndl_load_callback);
    if (rc != 0) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR, "NDL_DirectMediaLoad failed rc=%d error=%s", rc,
                        backend_ndl_last_error_locked(ctx));
        return BACKEND_NDL_ERROR;
    }

    ctx->media_loaded = true;
    clock_gettime(CLOCK_MONOTONIC, &ctx->loaded_time);
    ctx->reload_generation++;
    ctx->need_keyframe = ctx->has_video && ctx->config.require_keyframe_after_reload;

    if (ctx->config.video_frame_drop_threshold >= 0 && ctx->api.DirectVideoSetFrameDropThreshold) {
        int frc = ctx->api.DirectVideoSetFrameDropThreshold(ctx->config.video_frame_drop_threshold);
        if (frc != 0) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_WARN,
                            "NDL_DirectVideoSetFrameDropThreshold(%d) failed rc=%d error=%s",
                            ctx->config.video_frame_drop_threshold, frc,
                            backend_ndl_last_error_locked(ctx));
        }
    }

    if (ctx->has_audio && ctx->config.prime_pcm_after_load) {
        /* Prime the PCM sink with one empty frame right after every load;
         * without it the firmware delays first audio output noticeably. */
        unsigned short empty_buf[2] = {0};
        unsigned int prime_size = (unsigned int)ctx->pcm.channels * (unsigned int)sizeof(unsigned short);
        long long prime_pts = backend_ndl_media_time_ms_locked(ctx);
        int arc = ctx->api.DirectAudioPlay(empty_buf, prime_size, prime_pts);
        if (arc != 0) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_WARN,
                            "PCM priming feed failed rc=%d error=%s (continuing)", arc,
                            backend_ndl_last_error_locked(ctx));
        } else {
            BACKEND_NDL_DEBUG_LOG(ctx, "primed PCM sink: %u bytes pts=%lld", prime_size, prime_pts);
        }
    }

    BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "media loaded: generation=%llu video=%s audio=%s",
                    (unsigned long long)ctx->reload_generation, ctx->has_video ? "yes" : "no",
                    ctx->has_audio ? "yes" : "no");
    return BACKEND_NDL_OK;
}

/* A failed atomic reconfigure has already unloaded the pipeline. After the
 * caller's state rollback, bring the previous configuration back up. */
static void backend_ndl_restore_after_failed_reload_locked(BackendNdl *ctx) {
    if (ctx->media_loaded || (!ctx->has_video && !ctx->has_audio)) {
        return;
    }
    BackendNdlResult restore = backend_ndl_reload_locked(ctx);
    if (restore == BACKEND_NDL_OK) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO,
                        "restored previous media configuration after failed reconfigure");
    } else {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR,
                        "could not restore previous media configuration after failed reconfigure: %s",
                        backend_ndl_result_name(restore));
    }
}

static BackendNdl *backend_ndl_open_common(const BackendNdlConfig *config, const BackendNdlApi *api) {
    if (!config || !config->app_id || config->app_id[0] == '\0' ||
        config->minimum_log_level < BACKEND_NDL_LOG_DEBUG ||
        config->minimum_log_level > BACKEND_NDL_LOG_OFF) {
        backend_ndl_log_config(config, BACKEND_NDL_LOG_ERROR,
                               "config with a non-empty app_id is required");
        return NULL;
    }
    if (g_backend_ndl_active) {
        backend_ndl_log_config(config, BACKEND_NDL_LOG_ERROR,
                               "backend already open; NDL DirectMedia is a per-process singleton");
        return NULL;
    }

    BackendNdl *ctx = (BackendNdl *)calloc(1, sizeof(BackendNdl));
    if (!ctx) {
        return NULL;
    }
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        free(ctx);
        return NULL;
    }

    ctx->config = *config;

    ctx->app_id = backend_ndl_strdup(config->app_id);
    if (!ctx->app_id) {
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return NULL;
    }

    pthread_mutex_lock(&g_backend_ndl_registry_lock);
    g_backend_ndl_active = ctx;
    pthread_mutex_unlock(&g_backend_ndl_registry_lock);

    if (api) {
        ctx->api = *api;
        if (!ctx->api.DirectMediaGetError || !ctx->api.DirectMediaInit || !ctx->api.DirectMediaQuit ||
            !ctx->api.DirectMediaLoad || !ctx->api.DirectMediaUnload || !ctx->api.DirectVideoPlay ||
            !ctx->api.DirectAudioPlay) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR,
                            "open_with_api: a required entry point is NULL");
            backend_ndl_close(ctx);
            return NULL;
        }
    } else {
        if (!backend_ndl_open_library(ctx) || !backend_ndl_resolve_api(ctx)) {
            backend_ndl_close(ctx);
            return NULL;
        }
    }

    if (ctx->config.call_dl_initialize && ctx->api.DirectMedia_DL_Initialize &&
        (!ctx->api.DirectMedia_DL_IsInitialized || !ctx->api.DirectMedia_DL_IsInitialized())) {
        if (!ctx->api.DirectMedia_DL_Initialize()) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR, "NDL_DirectMedia_DL_Initialize failed");
            backend_ndl_close(ctx);
            return NULL;
        }
        ctx->ndl_dl_initialized = true;
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "NDL dynamic implementation initialized");
    }

    int rc = ctx->api.DirectMediaInit(ctx->app_id, backend_ndl_resource_released_callback);
    if (rc != 0) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR,
                        "NDL_DirectMediaInit(%s) failed rc=%d error=%s", ctx->app_id, rc,
                        backend_ndl_last_error_locked(ctx));
        backend_ndl_close(ctx);
        return NULL;
    }
    ctx->ndl_initialized = true;
    BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO, "initialized app_id=%s", ctx->app_id);
    return ctx;
}

BackendNdl *backend_ndl_open(const BackendNdlConfig *config) {
    return backend_ndl_open_common(config, NULL);
}

#if defined(BACKEND_NDL_TESTING) && BACKEND_NDL_TESTING
BackendNdl *backend_ndl_open_with_api(const BackendNdlConfig *config, const BackendNdlApi *api) {
    if (!api) {
        return NULL;
    }
    return backend_ndl_open_common(config, api);
}
#endif

void backend_ndl_close(BackendNdl *ctx) {
    if (!ctx) {
        return;
    }

    /* Unregister FIRST: taking the registry lock waits out any firmware
     * callback still running, and every later one (including the
     * UNLOADCOMPLETED our own unload below triggers) sees no owner and is
     * dropped — nothing can dereference the context during the teardown. */
    pthread_mutex_lock(&g_backend_ndl_registry_lock);
    if (g_backend_ndl_active == ctx) {
        g_backend_ndl_active = NULL;
    }
    pthread_mutex_unlock(&g_backend_ndl_registry_lock);

    pthread_mutex_lock(&ctx->lock);

    if (ctx->media_loaded) {
        (void)backend_ndl_unload_locked(ctx);
    }

    if (ctx->ndl_initialized && ctx->api.DirectMediaQuit) {
        int rc = ctx->api.DirectMediaQuit();
        if (rc != 0) {
            BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR,
                            "NDL_DirectMediaQuit failed rc=%d error=%s", rc,
                            backend_ndl_last_error_locked(ctx));
        }
        ctx->ndl_initialized = false;
    }

    if (ctx->ndl_dl_initialized && ctx->api.DirectMedia_DL_Finalize) {
        ctx->api.DirectMedia_DL_Finalize();
        ctx->ndl_dl_initialized = false;
    }

    pthread_mutex_unlock(&ctx->lock);

    if (ctx->dl_handle && !ctx->dl_handle_is_default) {
        dlclose(ctx->dl_handle);
        ctx->dl_handle = NULL;
    }

    free(ctx->app_id);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

BackendNdlResult backend_ndl_set_media(BackendNdl *ctx,
                                      const BackendNdlVideoInfo *video,
                                      const BackendNdlPcmInfo *audio) {
    if (!ctx) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    if (video) {
        if (video->codec == BACKEND_NDL_VIDEO_NONE || video->width == 0 || video->height == 0 ||
            video->width > (uintmax_t)INT_MAX || video->height > (uintmax_t)INT_MAX) {
            return BACKEND_NDL_INVALID_ARGUMENT;
        }
        if (video->codec < BACKEND_NDL_VIDEO_H264 || video->codec > BACKEND_NDL_VIDEO_AV1) {
            return BACKEND_NDL_UNSUPPORTED;
        }
    }
    if (audio) {
        if (audio->sample_rate_hz == 0 || audio->channels == 0) {
            return BACKEND_NDL_INVALID_ARGUMENT;
        }
        if (!backend_ndl_pcm_channel_mode(audio->channels) ||
            !backend_ndl_pcm_sample_rate_supported(audio->sample_rate_hz)) {
            return BACKEND_NDL_UNSUPPORTED;
        }
    }

    pthread_mutex_lock(&ctx->lock);
    BackendNdlVideoInfo saved_video = ctx->video;
    BackendNdlPcmInfo saved_audio = ctx->pcm;
    bool had_video = ctx->has_video;
    bool had_audio = ctx->has_audio;

    memset(&ctx->video, 0, sizeof(ctx->video));
    memset(&ctx->pcm, 0, sizeof(ctx->pcm));
    ctx->has_video = video != NULL;
    ctx->has_audio = audio != NULL;
    if (video) {
        ctx->video = *video;
    }
    if (audio) {
        ctx->pcm = *audio;
    }

    BackendNdlResult result = backend_ndl_reload_locked(ctx);
    if (result != BACKEND_NDL_OK) {
        ctx->video = saved_video;
        ctx->pcm = saved_audio;
        ctx->has_video = had_video;
        ctx->has_audio = had_audio;
        backend_ndl_restore_after_failed_reload_locked(ctx);
    }
    pthread_mutex_unlock(&ctx->lock);
    return result;
}

BackendNdlResult backend_ndl_unload(BackendNdl *ctx) {
    return backend_ndl_set_media(ctx, NULL, NULL);
}

BackendNdlResult backend_ndl_reload(BackendNdl *ctx) {
    if (!ctx) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&ctx->lock);
    BackendNdlResult result = backend_ndl_reload_locked(ctx);
    pthread_mutex_unlock(&ctx->lock);
    return result;
}

BackendNdlResult backend_ndl_feed_video(BackendNdl *ctx,
                                        const void *data,
                                        size_t len,
                                        long long ndl_pts,
                                        bool is_keyframe) {
    if (!ctx || !data || len == 0 || len > UINT32_MAX) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&ctx->lock);

    if (!ctx->ndl_initialized || !ctx->media_loaded || !ctx->has_video) {
        pthread_mutex_unlock(&ctx->lock);
        return BACKEND_NDL_NOT_READY;
    }

    if (ctx->need_keyframe && !is_keyframe) {
        pthread_mutex_unlock(&ctx->lock);
        return BACKEND_NDL_NEED_KEYFRAME;
    }

    int rc = ctx->api.DirectVideoPlay((void *)data, (unsigned int)len, ndl_pts);
    if (rc != 0) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR,
                        "NDL_DirectVideoPlay failed rc=%d len=%zu error=%s", rc, len,
                        backend_ndl_last_error_locked(ctx));
        pthread_mutex_unlock(&ctx->lock);
        return BACKEND_NDL_ERROR;
    }

    if (is_keyframe) {
        ctx->need_keyframe = false;
    }

    ctx->video_feed_count++;
    if (backend_ndl_log_enabled(ctx, BACKEND_NDL_LOG_DEBUG) &&
        ctx->video_feed_count % BACKEND_NDL_VIDEO_DEBUG_CADENCE == 0 &&
        ctx->api.DirectVideoGetRenderBufferLength) {
        int depth = -1;
        if (ctx->api.DirectVideoGetRenderBufferLength(&depth) == 0) {
            BACKEND_NDL_DEBUG_LOG(ctx, "video sink: fed=%llu render_queue=%d pts=%lld",
                                  (unsigned long long)ctx->video_feed_count, depth, ndl_pts);
        }
    }

    pthread_mutex_unlock(&ctx->lock);
    return BACKEND_NDL_OK;
}

BackendNdlResult backend_ndl_feed_audio(BackendNdl *ctx,
                                        const void *data,
                                        size_t len,
                                        long long ndl_pts) {
    if (!ctx || !data || len == 0 || len > UINT32_MAX) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&ctx->lock);

    if (!ctx->ndl_initialized || !ctx->media_loaded || !ctx->has_audio) {
        ctx->audio_not_ready_streak++;
        pthread_mutex_unlock(&ctx->lock);
        return BACKEND_NDL_NOT_READY;
    }

    if (ctx->config.guard_audio_overflow && ctx->api.DirectAudioGetAvailableBufferSize) {
        int available = -1;
        int arc = ctx->api.DirectAudioGetAvailableBufferSize(&available);
        if (arc == 0 && available >= 0 && len > (size_t)available) {
            ctx->audio_overflow_streak++;
            BACKEND_NDL_DEBUG_LOG(ctx, "audio overflow drop #%u: len=%zu available=%d",
                                  (unsigned)ctx->audio_overflow_streak, len, available);
            pthread_mutex_unlock(&ctx->lock);
            return BACKEND_NDL_OVERFLOW;
        }
    }

    int rc = ctx->api.DirectAudioPlay((void *)data, (unsigned int)len, ndl_pts);
    if (rc != 0) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_ERROR,
                        "NDL_DirectAudioPlay failed rc=%d len=%zu error=%s", rc, len,
                        backend_ndl_last_error_locked(ctx));
        pthread_mutex_unlock(&ctx->lock);
        return BACKEND_NDL_ERROR;
    }

    /* Recovery summary makes drop episodes conclusive for the caller. */
    if (ctx->audio_overflow_streak > 0 || ctx->audio_not_ready_streak > 0) {
        BACKEND_NDL_LOG(ctx, BACKEND_NDL_LOG_INFO,
                        "audio sink recovered: dropped %u block(s) on overflow, %u while not ready",
                        (unsigned)ctx->audio_overflow_streak,
                        (unsigned)ctx->audio_not_ready_streak);
        ctx->audio_overflow_streak = 0;
        ctx->audio_not_ready_streak = 0;
    }

    ctx->audio_feed_count++;
    if (backend_ndl_log_enabled(ctx, BACKEND_NDL_LOG_DEBUG) &&
        ctx->audio_feed_count % BACKEND_NDL_AUDIO_DEBUG_CADENCE == 0 &&
        ctx->api.DirectAudioGetAvailableBufferSize) {
        int available = -1;
        int total = -1;
        if (ctx->api.DirectAudioGetAvailableBufferSize(&available) == 0) {
            if (ctx->api.DirectAudioGetTotalBufferSize) {
                (void)ctx->api.DirectAudioGetTotalBufferSize(&total);
            }
            BACKEND_NDL_DEBUG_LOG(ctx, "audio sink: fed=%llu available=%d total=%d pts=%lld",
                                  (unsigned long long)ctx->audio_feed_count, available, total, ndl_pts);
        }
    }

    pthread_mutex_unlock(&ctx->lock);
    return BACKEND_NDL_OK;
}

long long backend_ndl_media_time_ms(const BackendNdl *ctx) {
    if (!ctx) {
        return 0;
    }
    BackendNdl *mutable_ctx = (BackendNdl *)ctx;
    pthread_mutex_lock(&mutable_ctx->lock);
    long long ms = backend_ndl_media_time_ms_locked(ctx);
    pthread_mutex_unlock(&mutable_ctx->lock);
    return ms;
}

/* Optional-symbol passthroughs. */

static BackendNdlResult backend_ndl_opt_get_int(BackendNdl *ctx, int (*fn)(int *), int *out) {
    if (!ctx || !out) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    if (!fn) {
        return BACKEND_NDL_UNAVAILABLE;
    }
    pthread_mutex_lock(&ctx->lock);
    int rc = fn(out);
    pthread_mutex_unlock(&ctx->lock);
    return rc == 0 ? BACKEND_NDL_OK : BACKEND_NDL_ERROR;
}

BackendNdlResult backend_ndl_get_audio_available(BackendNdl *ctx, int *available_bytes) {
    return backend_ndl_opt_get_int(ctx, ctx ? ctx->api.DirectAudioGetAvailableBufferSize : NULL, available_bytes);
}

BackendNdlResult backend_ndl_get_audio_total(BackendNdl *ctx, int *total_bytes) {
    return backend_ndl_opt_get_int(ctx, ctx ? ctx->api.DirectAudioGetTotalBufferSize : NULL, total_bytes);
}

BackendNdlResult backend_ndl_get_video_render_buffer_length(BackendNdl *ctx, int *frames) {
    return backend_ndl_opt_get_int(ctx, ctx ? ctx->api.DirectVideoGetRenderBufferLength : NULL, frames);
}

BackendNdlResult backend_ndl_flush_video_render_buffer(BackendNdl *ctx) {
    if (!ctx) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    if (!ctx->api.DirectVideoFlushRenderBuffer) {
        return BACKEND_NDL_UNAVAILABLE;
    }
    pthread_mutex_lock(&ctx->lock);
    int rc = ctx->api.DirectVideoFlushRenderBuffer();
    pthread_mutex_unlock(&ctx->lock);
    return rc == 0 ? BACKEND_NDL_OK : BACKEND_NDL_ERROR;
}

BackendNdlResult backend_ndl_set_video_area(BackendNdl *ctx, int left, int top, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    if (!ctx->api.DirectVideoSetArea) {
        return BACKEND_NDL_UNAVAILABLE;
    }
    pthread_mutex_lock(&ctx->lock);
    int rc = ctx->api.DirectVideoSetArea(left, top, width, height);
    pthread_mutex_unlock(&ctx->lock);
    return rc == 0 ? BACKEND_NDL_OK : BACKEND_NDL_ERROR;
}

BackendNdlResult backend_ndl_set_foreground(BackendNdl *ctx, bool foreground) {
    if (!ctx) {
        return BACKEND_NDL_INVALID_ARGUMENT;
    }
    if (!ctx->api.DirectMediaSetAppState) {
        return BACKEND_NDL_UNAVAILABLE;
    }
    pthread_mutex_lock(&ctx->lock);
    int rc = ctx->api.DirectMediaSetAppState(foreground ? NDL_DIRECTMEDIA_APP_STATE_FOREGROUND
                                                        : NDL_DIRECTMEDIA_APP_STATE_BACKGROUND);
    pthread_mutex_unlock(&ctx->lock);
    return rc == 0 ? BACKEND_NDL_OK : BACKEND_NDL_ERROR;
}

bool backend_ndl_pcm_sample_rate_supported(uint32_t hz) {
    return hz <= INT32_MAX &&
           NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF((int)hz) != NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_NONE;
}

#else /* !BACKEND_NDL_ENABLED */

BackendNdl *backend_ndl_open(const BackendNdlConfig *config) {
    backend_ndl_log_config(config, BACKEND_NDL_LOG_WARN,
                           "built without DirectMedia platform support; backend unavailable");
    return NULL;
}

void backend_ndl_close(BackendNdl *ctx) {
    (void)ctx;
}

BackendNdlResult backend_ndl_set_media(BackendNdl *ctx,
                                      const BackendNdlVideoInfo *video,
                                      const BackendNdlPcmInfo *audio) {
    (void)ctx;
    (void)video;
    (void)audio;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_unload(BackendNdl *ctx) {
    (void)ctx;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_reload(BackendNdl *ctx) {
    (void)ctx;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_feed_video(BackendNdl *ctx, const void *data, size_t len, long long ndl_pts,
                                        bool is_keyframe) {
    (void)ctx;
    (void)data;
    (void)len;
    (void)ndl_pts;
    (void)is_keyframe;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_feed_audio(BackendNdl *ctx, const void *data, size_t len, long long ndl_pts) {
    (void)ctx;
    (void)data;
    (void)len;
    (void)ndl_pts;
    return BACKEND_NDL_UNAVAILABLE;
}

long long backend_ndl_media_time_ms(const BackendNdl *ctx) {
    (void)ctx;
    return 0;
}

BackendNdlResult backend_ndl_get_audio_available(BackendNdl *ctx, int *available_bytes) {
    (void)ctx;
    (void)available_bytes;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_get_audio_total(BackendNdl *ctx, int *total_bytes) {
    (void)ctx;
    (void)total_bytes;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_get_video_render_buffer_length(BackendNdl *ctx, int *frames) {
    (void)ctx;
    (void)frames;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_flush_video_render_buffer(BackendNdl *ctx) {
    (void)ctx;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_set_video_area(BackendNdl *ctx, int left, int top, int width, int height) {
    (void)ctx;
    (void)left;
    (void)top;
    (void)width;
    (void)height;
    return BACKEND_NDL_UNAVAILABLE;
}

BackendNdlResult backend_ndl_set_foreground(BackendNdl *ctx, bool foreground) {
    (void)ctx;
    (void)foreground;
    return BACKEND_NDL_UNAVAILABLE;
}

bool backend_ndl_pcm_sample_rate_supported(uint32_t hz) {
    (void)hz;
    return false;
}

#endif /* BACKEND_NDL_ENABLED */
