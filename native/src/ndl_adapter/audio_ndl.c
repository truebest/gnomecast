#include "audio_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "rdp_ffi.h"

#ifdef HELLOLG_WITH_NDL
#include "media_ndl_internal.h"
#endif

#include "clog.h"

clog_define(g_native_log_audio, cLogLevelInfo, cLogFlags_Default, "audio.ndl", NULL);

struct NativeAudio {
#ifdef HELLOLG_WITH_NDL
    /* Borrowed from the caller; the track never closes either object. */
    NativeMedia *media;
    BackendNdl *backend;
    bool audio_opened;
    /* Muted after a feed error: the track stays open so live video is not forced
     * through another pipeline reload and keyframe recovery. */
    bool disabled;
    bool not_ready_logged;
    bool overflow_logged;
#else
    int unused;
#endif
};

NativeAudio *native_audio_open(NativeMedia *media, uint32_t codec, uint32_t sample_rate, uint16_t channels) {
#ifndef HELLOLG_WITH_NDL
    (void)media;
    (void)codec;
    (void)sample_rate;
    (void)channels;
    clog(cLogLevelWarning, "NDL backend is not linked; audio unavailable");
    return NULL;
#else
    if (sample_rate == 0 || channels == 0) {
        clog(cLogLevelWarning, "invalid audio format rate=%u channels=%u", (unsigned)sample_rate,
             (unsigned)channels);
        return NULL;
    }
    BackendNdl *backend = native_media_ndl_backend(media);
    if (!backend) {
        clog(cLogLevelWarning, "no shared media backend available");
        return NULL;
    }
    if (codec == RDP_AUDIO_CODEC_OPUS) {
        /* Deliberate: the mixer upstream decodes every Opus stream to PCM, and the
         * NDL Opus passthrough ABI (sampleRate units) is undocumented. */
        clog(cLogLevelWarning, "NDL backend has no Opus passthrough; upstream decodes to PCM");
        return NULL;
    }
    if (codec != RDP_AUDIO_CODEC_PCM_S16LE) {
        clog(cLogLevelWarning, "unknown codec id %u", (unsigned)codec);
        return NULL;
    }
    if (!backend_ndl_pcm_sample_rate_supported(sample_rate)) {
        clog(cLogLevelWarning, "NDL does not support %u Hz PCM", (unsigned)sample_rate);
        return NULL;
    }
    if (channels > 2) {
        clog(cLogLevelWarning, "%u channels exceed the NDL PCM backend maximum 2", (unsigned)channels);
        return NULL;
    }

    NativeAudio *audio = (NativeAudio *)calloc(1, sizeof(NativeAudio));
    if (!audio) {
        return NULL;
    }
    audio->media = media;
    audio->backend = backend;

    BackendNdlPcmInfo info = {
        .sample_rate_hz = sample_rate,
        .channels = channels,
    };
    /* Reloads the shared pipeline (unload+load with the combined tracks) and primes
     * the PCM sink; a live video track needs a fresh keyframe afterwards. */
    BackendNdlResult result = native_media_ndl_configure_audio(media, &info);
    if (result != BACKEND_NDL_OK) {
        clog(cLogLevelWarning, "NDL failed to open PCM stream: %s", backend_ndl_result_name(result));
        free(audio);
        return NULL;
    }
    audio->audio_opened = true;
    clog(cLogLevelInfo, "opened PCM %uHz %uch sink (NDL)", (unsigned)sample_rate, (unsigned)channels);
    return audio;
#endif
}

NativeAudioResult native_audio_feed(NativeAudio *audio, const uint8_t *data, size_t len) {
#ifndef HELLOLG_WITH_NDL
    (void)audio;
    (void)data;
    (void)len;
    return NATIVE_AUDIO_ERROR;
#else
    if (!audio || !audio->audio_opened || !data || len == 0) {
        return NATIVE_AUDIO_ERROR;
    }
    if (audio->disabled) {
        return NATIVE_AUDIO_OK;
    }

    switch (backend_ndl_feed_audio(audio->backend, data, len, backend_ndl_media_time_ms(audio->backend))) {
        case BACKEND_NDL_OK:
            audio->not_ready_logged = false;
            audio->overflow_logged = false;
            return NATIVE_AUDIO_OK;
        case BACKEND_NDL_NOT_READY:
            /* Transient (e.g. during a pipeline reload when video opens); drop. */
            if (!audio->not_ready_logged) {
                clog(cLogLevelWarning, "NDL audio feed not ready; dropping audio until it recovers");
                audio->not_ready_logged = true;
            }
            return NATIVE_AUDIO_OK;
        case BACKEND_NDL_OVERFLOW:
            if (!audio->overflow_logged) {
                clog(cLogLevelWarning, "NDL audio buffer overflow; dropping audio until it drains");
                audio->overflow_logged = true;
            }
            return NATIVE_AUDIO_OK;
        default:
            clog(cLogLevelWarning, "NDL audio feed returned error");
            return NATIVE_AUDIO_ERROR;
    }
#endif
}

void native_audio_disable(NativeAudio *audio) {
    if (!audio) {
        return;
    }
#ifdef HELLOLG_WITH_NDL
    if (!audio->disabled) {
        clog(cLogLevelNotice, "audio muted; keeping the shared media pipeline intact");
        audio->disabled = true;
    }
#endif
}

void native_audio_close(NativeAudio *audio) {
    if (!audio) {
        return;
    }
#ifdef HELLOLG_WITH_NDL
    /* The media adapter reloads the surviving video track atomically. */
    if (audio->media && audio->backend && audio->audio_opened) {
        (void)native_media_ndl_clear_audio(audio->media);
    }
#endif
    free(audio);
}
