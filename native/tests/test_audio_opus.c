#include <assert.h>
#include <math.h>
#include <opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_opus.h"

/* Round-trip through libopus: encode a sine, decode with the wrapper, verify shape. */

#define TEST_RATE 48000
#define TEST_CHANNELS 2
#define TEST_FRAMES 960 /* 20ms @48k — what gnome-remote-desktop packs per wave */

static void fill_sine(int16_t *pcm, int frames) {
    for (int i = 0; i < frames; i++) {
        int16_t sample = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * i / TEST_RATE));
        pcm[i * 2] = sample;
        pcm[i * 2 + 1] = sample;
    }
}

static void test_round_trip(void) {
    int error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(TEST_RATE, TEST_CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    assert(encoder && error == OPUS_OK);

    NativeOpusDecoder *decoder = native_opus_decoder_open(TEST_RATE, TEST_CHANNELS);
    assert(decoder);

    int16_t pcm_in[TEST_FRAMES * TEST_CHANNELS];
    fill_sine(pcm_in, TEST_FRAMES);

    /* Several consecutive packets: the decoder keeps state between them. */
    long energy = 0;
    for (int packet = 0; packet < 5; packet++) {
        unsigned char payload[4000];
        opus_int32 len = opus_encode(encoder, pcm_in, TEST_FRAMES, payload, sizeof(payload));
        assert(len > 0);

        const int16_t *pcm_out = NULL;
        int frames = native_opus_decoder_decode(decoder, payload, (size_t)len, &pcm_out);
        assert(frames == TEST_FRAMES);
        assert(pcm_out != NULL);
        for (int i = 0; i < frames * TEST_CHANNELS; i++) {
            energy += labs((long)pcm_out[i]);
        }
    }
    assert(energy > 0); /* decoded audio is not silence */

    native_opus_decoder_close(decoder);
    opus_encoder_destroy(encoder);
}

static void test_garbage_is_skipped(void) {
    NativeOpusDecoder *decoder = native_opus_decoder_open(TEST_RATE, TEST_CHANNELS);
    assert(decoder);
    const uint8_t garbage[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22};
    const int16_t *pcm = NULL;
    /* Corrupt packets must be skipped (0), never crash or return frames. */
    for (int i = 0; i < 3; i++) {
        assert(native_opus_decoder_decode(decoder, garbage, sizeof(garbage), &pcm) == 0);
    }
    native_opus_decoder_close(decoder);
}

static void test_null_safety(void) {
    const int16_t *pcm = NULL;
    assert(native_opus_decoder_decode(NULL, (const uint8_t *)"x", 1, &pcm) == 0);
    native_opus_decoder_close(NULL);
    assert(native_opus_decoder_open(TEST_RATE, 0) == NULL);
    assert(native_opus_decoder_open(TEST_RATE, 3) == NULL);
}

int main(void) {
    test_round_trip();
    test_garbage_is_skipped();
    test_null_safety();
    printf("test_audio_opus: all tests passed\n");
    return 0;
}
