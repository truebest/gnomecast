#include "audio_ss4s.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdp_ffi.h"

#ifdef HELLOLG_WITH_SS4S
#include <ss4s.h>
#endif

/* Upper bound on queued prebuffer packets (safety valve; ~64 Opus packets = 1.28s). */
#define NATIVE_AUDIO_PREBUFFER_MAX_PACKETS 64
/* gnome-remote-desktop encodes Opus at 20ms per packet; used to convert the configured
 * prebuffer milliseconds into a packet budget when the payload itself has no timing. */
#define NATIVE_AUDIO_OPUS_PACKET_MS 20u
/* Arrival gaps above this are logged as jitter evidence (rate-limited). */
#define NATIVE_AUDIO_JITTER_LOG_THRESHOLD_MS 60u

struct NativeAudio {
#ifdef HELLOLG_WITH_SS4S
    /* Borrowed from the caller's NativeMedia; the track never closes the player. */
    SS4S_Player *player;
    bool audio_opened;
    /* Muted after a feed error: the track stays open (closing it would unload the shared
     * pipeline and freeze video, see native_audio_disable) but feeds are dropped. */
    bool disabled;
    bool not_ready_logged;
    bool overflow_logged;
    uint32_t codec;
    uint32_t sample_rate;
    uint16_t channels;
    /* Jitter buffer: audio shares one TCP connection with video, so packets can stall
     * behind large video frames and arrive in a late burst; feeding only after this much
     * audio is queued keeps the hardware playing through such stalls. */
    uint32_t prebuffer_ms;
    bool prebuffering;
    uint8_t *queued[NATIVE_AUDIO_PREBUFFER_MAX_PACKETS];
    size_t queued_len[NATIVE_AUDIO_PREBUFFER_MAX_PACKETS];
    int queued_count;
    uint32_t queued_ms;
    uint64_t last_arrival_ms;
    uint64_t last_jitter_log_ms;
#else
    int unused;
#endif
};

#ifdef HELLOLG_WITH_SS4S
static SS4S_AudioCodec native_audio_ss4s_codec(uint32_t codec) {
    switch (codec) {
        case RDP_AUDIO_CODEC_OPUS:
            return SS4S_AUDIO_OPUS;
        case RDP_AUDIO_CODEC_PCM_S16LE:
            return SS4S_AUDIO_PCM_S16LE;
        default:
            return SS4S_AUDIO_NONE;
    }
}

static uint64_t native_audio_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Estimated playback duration of one fed packet. Exact for PCM; gnome-remote-desktop's
 * fixed packetization for Opus. */
static uint32_t native_audio_packet_ms(const NativeAudio *audio, size_t len) {
    if (audio->codec == RDP_AUDIO_CODEC_PCM_S16LE && audio->sample_rate != 0 && audio->channels != 0) {
        uint64_t frames = (uint64_t)len / ((uint64_t)audio->channels * 2u);
        return (uint32_t)(frames * 1000u / audio->sample_rate);
    }
    return NATIVE_AUDIO_OPUS_PACKET_MS;
}

static void native_audio_drop_queue(NativeAudio *audio) {
    for (int i = 0; i < audio->queued_count; i++) {
        free(audio->queued[i]);
        audio->queued[i] = NULL;
    }
    audio->queued_count = 0;
    audio->queued_ms = 0;
}

static NativeAudioResult native_audio_feed_hw(NativeAudio *audio, const uint8_t *data, size_t len) {
    switch (SS4S_PlayerAudioFeed(audio->player, data, len)) {
        case SS4S_AUDIO_FEED_OK:
            audio->not_ready_logged = false;
            audio->overflow_logged = false;
            return NATIVE_AUDIO_OK;
        case SS4S_AUDIO_FEED_NOT_READY:
            /* Transient (e.g. during the NDL pipeline reload when video opens); drop. */
            if (!audio->not_ready_logged) {
                fprintf(stderr, "[native-audio] ss4s audio feed not ready; dropping audio until it recovers\n");
                audio->not_ready_logged = true;
            }
            return NATIVE_AUDIO_OK;
        case SS4S_AUDIO_FEED_OVERFLOW:
            if (!audio->overflow_logged) {
                fprintf(stderr, "[native-audio] ss4s audio buffer overflow; dropping audio until it drains\n");
                audio->overflow_logged = true;
            }
            return NATIVE_AUDIO_OK;
        case SS4S_AUDIO_FEED_ERROR:
        default:
            fprintf(stderr, "[native-audio] ss4s audio feed returned error\n");
            return NATIVE_AUDIO_ERROR;
    }
}

static NativeAudioResult native_audio_flush_queue(NativeAudio *audio) {
    NativeAudioResult result = NATIVE_AUDIO_OK;
    for (int i = 0; i < audio->queued_count; i++) {
        if (result == NATIVE_AUDIO_OK) {
            result = native_audio_feed_hw(audio, audio->queued[i], audio->queued_len[i]);
        }
        free(audio->queued[i]);
        audio->queued[i] = NULL;
    }
    audio->queued_count = 0;
    audio->queued_ms = 0;
    return result;
}
#endif

NativeAudio *native_audio_open(NativeMedia *media, uint32_t codec, uint32_t sample_rate, uint16_t channels,
                               uint32_t prebuffer_ms) {
#ifndef HELLOLG_WITH_SS4S
    (void)media;
    (void)codec;
    (void)sample_rate;
    (void)channels;
    (void)prebuffer_ms;
    fprintf(stderr, "[native-audio] ss4s is not linked; audio unavailable\n");
    return NULL;
#else
    if (sample_rate == 0 || channels == 0) {
        fprintf(stderr, "[native-audio] invalid audio format rate=%u channels=%u\n", (unsigned)sample_rate,
                (unsigned)channels);
        return NULL;
    }
    SS4S_Player *player = native_media_player(media);
    if (!player) {
        fprintf(stderr, "[native-audio] no shared media player available\n");
        return NULL;
    }
    SS4S_AudioCodec ss4s_codec = native_audio_ss4s_codec(codec);
    if (ss4s_codec == SS4S_AUDIO_NONE) {
        fprintf(stderr, "[native-audio] unknown codec id %u\n", (unsigned)codec);
        return NULL;
    }

    SS4S_AudioCapabilities capabilities;
    memset(&capabilities, 0, sizeof(capabilities));
    /* Must be the ByCodecs variant: the parameterless SS4S_GetAudioCapabilities passes
     * SS4S_AUDIO_NONE as the wanted set, which the ndl module treats as "no match". */
    if (!SS4S_GetAudioCapabilitiesByCodecs(&capabilities, ss4s_codec) || (capabilities.codecs & ss4s_codec) == 0) {
        fprintf(stderr, "[native-audio] ss4s module does not support %s\n", SS4S_AudioCodecName(ss4s_codec));
        return NULL;
    }
    if (capabilities.maxChannels != 0 && channels > capabilities.maxChannels) {
        fprintf(stderr, "[native-audio] %u channels exceed ss4s module maximum %u\n", (unsigned)channels,
                capabilities.maxChannels);
        return NULL;
    }

    NativeAudio *audio = (NativeAudio *)calloc(1, sizeof(NativeAudio));
    if (!audio) {
        return NULL;
    }
    audio->player = player;
    audio->codec = codec;
    audio->sample_rate = sample_rate;
    audio->channels = channels;
    audio->prebuffer_ms = prebuffer_ms;
    audio->prebuffering = prebuffer_ms > 0;

    SS4S_AudioInfo info;
    memset(&info, 0, sizeof(info));
    info.codec = ss4s_codec;
    info.sampleRate = (int)sample_rate;
    info.numOfChannels = (int)channels;
    /* Ignored by the NDL/SMP hardware modules (only software mixers size buffers from
     * it); 20ms at 48kHz for Opus, gnome-remote-desktop's PCM packet size otherwise. */
    info.samplesPerFrame = ss4s_codec == SS4S_AUDIO_OPUS ? 960 : 1024;
    info.appName = "gnomecast-native";
    info.streamName = "rdp-audio";

    switch (SS4S_PlayerAudioOpen(player, &info)) {
        case SS4S_AUDIO_OPEN_OK:
            audio->audio_opened = true;
            fprintf(stderr, "[native-audio] negotiated %s %uHz %uch (jitter prebuffer %ums)\n",
                    SS4S_AudioCodecName(ss4s_codec), (unsigned)sample_rate, (unsigned)channels,
                    (unsigned)prebuffer_ms);
            return audio;
        case SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC:
            fprintf(stderr, "[native-audio] ss4s rejected %s as unsupported\n", SS4S_AudioCodecName(ss4s_codec));
            break;
        case SS4S_AUDIO_OPEN_ERROR:
        default:
            fprintf(stderr, "[native-audio] ss4s failed to open %s stream\n", SS4S_AudioCodecName(ss4s_codec));
            break;
    }
    free(audio);
    return NULL;
#endif
}

NativeAudioResult native_audio_feed(NativeAudio *audio, const uint8_t *data, size_t len) {
#ifndef HELLOLG_WITH_SS4S
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

    uint64_t now_ms = native_audio_now_ms();
    if (audio->prebuffer_ms > 0 && audio->last_arrival_ms != 0 && now_ms > audio->last_arrival_ms) {
        uint64_t gap_ms = now_ms - audio->last_arrival_ms;
        if (gap_ms > (uint64_t)audio->prebuffer_ms + 200u) {
            /* Source pause (silence on the desktop) or a stall longer than the safety
             * margin: the hardware buffer has drained, so accumulate a fresh margin
             * before resuming playback. */
            if (!audio->prebuffering) {
                fprintf(stderr, "[native-audio] source gap ~%ums; rebuffering %ums\n", (unsigned)gap_ms,
                        (unsigned)audio->prebuffer_ms);
                audio->prebuffering = true;
            }
        } else if (gap_ms >= NATIVE_AUDIO_JITTER_LOG_THRESHOLD_MS &&
                   now_ms - audio->last_jitter_log_ms > 5000u) {
            /* Diagnostic evidence for dropout reports: bursts of video traffic delaying
             * audio on the shared connection show up as arrival gaps here. */
            fprintf(stderr, "[native-audio] arrival jitter ~%ums (absorbed by %ums prebuffer)\n", (unsigned)gap_ms,
                    (unsigned)audio->prebuffer_ms);
            audio->last_jitter_log_ms = now_ms;
        }
    }
    audio->last_arrival_ms = now_ms;

    if (!audio->prebuffering) {
        return native_audio_feed_hw(audio, data, len);
    }

    /* Prebuffering: copy the packet aside instead of feeding it. */
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        /* Degrade to direct feeding rather than losing audio entirely. */
        (void)native_audio_flush_queue(audio);
        audio->prebuffering = false;
        return native_audio_feed_hw(audio, data, len);
    }
    memcpy(copy, data, len);
    audio->queued[audio->queued_count] = copy;
    audio->queued_len[audio->queued_count] = len;
    audio->queued_count++;
    audio->queued_ms += native_audio_packet_ms(audio, len);

    if (audio->queued_ms >= audio->prebuffer_ms || audio->queued_count >= NATIVE_AUDIO_PREBUFFER_MAX_PACKETS) {
        audio->prebuffering = false;
        return native_audio_flush_queue(audio);
    }
    return NATIVE_AUDIO_OK;
#endif
}

void native_audio_disable(NativeAudio *audio) {
    if (!audio) {
        return;
    }
#ifdef HELLOLG_WITH_SS4S
    if (!audio->disabled) {
        fprintf(stderr, "[native-audio] audio muted; keeping the shared media pipeline intact\n");
        audio->disabled = true;
        native_audio_drop_queue(audio);
    }
#endif
}

void native_audio_close(NativeAudio *audio) {
    if (!audio) {
        return;
    }
#ifdef HELLOLG_WITH_SS4S
    /* NOTE: on the webOS backends SS4S_PlayerAudioClose unloads the shared media
     * pipeline (ndl: NDL_DirectMediaUnload, smp: StarfishPlayerUnloadInner) — it is NOT
     * track-only. Callers with a live video track must recover it afterwards. */
    native_audio_drop_queue(audio);
    if (audio->player && audio->audio_opened) {
        (void)SS4S_PlayerAudioClose(audio->player);
    }
#endif
    free(audio);
}
