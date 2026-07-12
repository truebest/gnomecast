#include "audio_opus.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(HELLOLG_WITH_OPUS) && HELLOLG_WITH_OPUS
#include <opus.h>
#endif

#include "clog.h"

clog_define(g_native_log_audio, cLogLevelInfo, cLogFlags_Default, "audio.opus", NULL);

#if defined(HELLOLG_WITH_OPUS) && HELLOLG_WITH_OPUS

/* 120ms at 48kHz — the largest frame an Opus packet may carry (MS-RDPEA/grd use 20ms,
 * but decode defensively). */
#define NATIVE_OPUS_MAX_FRAMES 5760

struct NativeOpusDecoder {
    OpusDecoder *decoder;
    uint16_t channels;
    unsigned decode_errors;
    int error_logged;
    int16_t pcm[NATIVE_OPUS_MAX_FRAMES * 2];
};

NativeOpusDecoder *native_opus_decoder_open(uint32_t sample_rate, uint16_t channels) {
    if (channels == 0 || channels > 2) {
        return NULL;
    }
    NativeOpusDecoder *decoder = (NativeOpusDecoder *)calloc(1, sizeof(NativeOpusDecoder));
    if (!decoder) {
        return NULL;
    }
    int error = OPUS_OK;
    decoder->decoder = opus_decoder_create((opus_int32)sample_rate, channels, &error);
    if (!decoder->decoder || error != OPUS_OK) {
        clog(cLogLevelWarning, "opus_decoder_create(%u, %u) failed: %s", (unsigned)sample_rate,
             (unsigned)channels, opus_strerror(error));
        free(decoder);
        return NULL;
    }
    decoder->channels = channels;
    return decoder;
}

int native_opus_decoder_decode(NativeOpusDecoder *decoder, const uint8_t *data, size_t len, const int16_t **pcm) {
    if (!decoder || !data || len == 0 || !pcm) {
        return 0;
    }
    int frames = opus_decode(decoder->decoder, data, (opus_int32)len, decoder->pcm, NATIVE_OPUS_MAX_FRAMES, 0);
    if (frames <= 0) {
        decoder->decode_errors++;
        if (!decoder->error_logged) {
            clog(cLogLevelWarning, "opus decode failed (%s); skipping bad packets",
                 frames < 0 ? opus_strerror(frames) : "empty");
            decoder->error_logged = 1;
        }
        return 0;
    }
    decoder->error_logged = 0;
    *pcm = decoder->pcm;
    return frames;
}

void native_opus_decoder_close(NativeOpusDecoder *decoder) {
    if (!decoder) {
        return;
    }
    if (decoder->decoder) {
        opus_decoder_destroy(decoder->decoder);
    }
    free(decoder);
}

#else /* !HELLOLG_WITH_OPUS */

NativeOpusDecoder *native_opus_decoder_open(uint32_t sample_rate, uint16_t channels) {
    (void)sample_rate;
    (void)channels;
    clog(cLogLevelWarning, "built without libopus; Opus sessions stay silent");
    return NULL;
}

int native_opus_decoder_decode(NativeOpusDecoder *decoder, const uint8_t *data, size_t len, const int16_t **pcm) {
    (void)decoder;
    (void)data;
    (void)len;
    (void)pcm;
    return 0;
}

void native_opus_decoder_close(NativeOpusDecoder *decoder) {
    (void)decoder;
}

#endif
