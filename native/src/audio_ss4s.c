#include "audio_ss4s.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdp_ffi.h"

#ifdef HELLOLG_WITH_SS4S
#include <ss4s.h>
#endif

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
#endif

NativeAudio *native_audio_open(NativeMedia *media, uint32_t codec, uint32_t sample_rate, uint16_t channels) {
#ifndef HELLOLG_WITH_SS4S
    (void)media;
    (void)codec;
    (void)sample_rate;
    (void)channels;
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
            fprintf(stderr, "[native-audio] negotiated %s %uHz %uch\n", SS4S_AudioCodecName(ss4s_codec),
                    (unsigned)sample_rate, (unsigned)channels);
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
    if (audio->player && audio->audio_opened) {
        (void)SS4S_PlayerAudioClose(audio->player);
    }
#endif
    free(audio);
}
