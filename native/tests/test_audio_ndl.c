#include "audio_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "backend_ndl_api.h"
#include "media_ndl_internal.h"
#include "rdp_ffi.h"

/* Exercises the gnomecast audio adapter over the real backend_ndl state machine with a scripted
 * fake NDL API. The media adapter is NOT linked: the test provides its own NativeMedia
 * and native_media_ndl_backend() double. */

static int expect_true(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

struct NativeMedia {
    BackendNdl *backend;
    bool has_audio;
    BackendNdlPcmInfo audio;
};

BackendNdl *native_media_ndl_backend(NativeMedia *media) {
    return media ? media->backend : NULL;
}

BackendNdlResult native_media_ndl_configure_audio(NativeMedia *media,
                                                   const BackendNdlPcmInfo *info) {
    media->has_audio = true;
    media->audio = *info;
    return backend_ndl_set_media(media->backend, NULL, &media->audio);
}

BackendNdlResult native_media_ndl_clear_audio(NativeMedia *media) {
    media->has_audio = false;
    memset(&media->audio, 0, sizeof(media->audio));
    return backend_ndl_set_media(media->backend, NULL, NULL);
}

typedef struct FakeNdl {
    int load_calls;
    int unload_calls;
    int audio_play_calls;
    bool fail_audio_play;
    int available_bytes;
    NDL_DIRECTMEDIA_DATA_INFO_T last_load;
} FakeNdl;

static FakeNdl g_fake;

static void fake_reset(void) {
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.available_bytes = 1 << 20;
}

static const char *fake_get_error(void) { return "fake error"; }
static int fake_media_init(const char *app_id, ResourceReleased cb) {
    (void)app_id;
    (void)cb;
    return 0;
}
static int fake_media_quit(void) { return 0; }
static int fake_media_load(NDL_DIRECTMEDIA_DATA_INFO_T *data, NDLMediaLoadCallback cb) {
    (void)cb;
    g_fake.load_calls++;
    g_fake.last_load = *data;
    return 0;
}
static int fake_media_unload(void) {
    g_fake.unload_calls++;
    return 0;
}
static int fake_video_play(void *buffer, unsigned int size, long long pts) {
    (void)buffer;
    (void)size;
    (void)pts;
    return 0;
}
static int fake_audio_play(void *buffer, unsigned int size, long long pts) {
    (void)buffer;
    (void)size;
    (void)pts;
    g_fake.audio_play_calls++;
    return g_fake.fail_audio_play ? -1 : 0;
}
static int fake_audio_available(int *available) {
    *available = g_fake.available_bytes;
    return 0;
}

static BackendNdl *open_fake_backend(void) {
    BackendNdlApi api;
    memset(&api, 0, sizeof(api));
    api.DirectMediaGetError = fake_get_error;
    api.DirectMediaInit = fake_media_init;
    api.DirectMediaQuit = fake_media_quit;
    api.DirectMediaLoad = fake_media_load;
    api.DirectMediaUnload = fake_media_unload;
    api.DirectVideoPlay = fake_video_play;
    api.DirectAudioPlay = fake_audio_play;
    api.DirectAudioGetAvailableBufferSize = fake_audio_available;
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    config.app_id = "test.audio.ndl";
    config.prime_pcm_after_load = true;
    return backend_ndl_open_with_api(&config, &api);
}

/* One 10 ms pump block: 480 frames of S16 stereo. */
static const uint8_t k_pcm_block[1920] = {0};

static int test_open_validation(void) {
    int failures = 0;
    fake_reset();
    struct NativeMedia media = {.backend = open_fake_backend()};

    failures += expect_true(native_audio_open(&media, RDP_AUDIO_CODEC_OPUS, 48000, 2) == NULL,
                            "Opus rejected (no passthrough)");
    failures += expect_true(native_audio_open(&media, RDP_AUDIO_CODEC_PCM_S16LE, 11025, 2) == NULL,
                            "unsupported sample rate rejected");
    failures += expect_true(native_audio_open(&media, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 3) == NULL,
                            "3 channels rejected");
    failures += expect_true(native_audio_open(&media, RDP_AUDIO_CODEC_PCM_S16LE, 0, 2) == NULL,
                            "zero rate rejected");
    failures += expect_true(g_fake.load_calls == 0, "rejected opens never load");
    failures += expect_true(native_audio_open(NULL, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 2) == NULL,
                            "NULL media rejected");

    NativeAudio *audio = native_audio_open(&media, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 2);
    failures += expect_true(audio != NULL, "PCM 48k stereo accepted");
    failures += expect_true(g_fake.load_calls == 1, "audio open loads the pipeline");
    failures += expect_true(g_fake.last_load.audio.pcm.type == NDL_AUDIO_TYPE_PCM &&
                            strcmp(g_fake.last_load.audio.pcm.channelMode, "stereo") == 0,
                            "PCM load config");
    failures += expect_true(g_fake.audio_play_calls == 1, "PCM sink primed after load");

    native_audio_close(audio);
    failures += expect_true(g_fake.unload_calls == 1, "audio close unloads the shared pipeline");
    backend_ndl_close(media.backend);
    return failures;
}

static int test_feed_semantics(void) {
    int failures = 0;
    fake_reset();
    struct NativeMedia media = {.backend = open_fake_backend()};
    NativeAudio *audio = native_audio_open(&media, RDP_AUDIO_CODEC_PCM_S16LE, 48000, 2);
    int primed = g_fake.audio_play_calls;

    failures += expect_true(native_audio_feed(audio, k_pcm_block, sizeof(k_pcm_block)) == NATIVE_AUDIO_OK,
                            "normal feed OK");
    failures += expect_true(g_fake.audio_play_calls == primed + 1, "feed reaches AudioPlay");

    /* Full hardware buffer: drop the block, still OK (the pump must never stall). */
    g_fake.available_bytes = (int)sizeof(k_pcm_block) - 1;
    failures += expect_true(native_audio_feed(audio, k_pcm_block, sizeof(k_pcm_block)) == NATIVE_AUDIO_OK,
                            "overflow drops but returns OK");
    failures += expect_true(g_fake.audio_play_calls == primed + 1, "overflow skips AudioPlay");
    g_fake.available_bytes = 1 << 20;

    /* Pipeline reloading underneath (e.g. video reopen): drop, still OK. */
    backend_ndl_unload(media.backend);
    failures += expect_true(native_audio_feed(audio, k_pcm_block, sizeof(k_pcm_block)) == NATIVE_AUDIO_OK,
                            "not-ready drops but returns OK");

    BackendNdlPcmInfo pcm = {.sample_rate_hz = 48000, .channels = 2};
    backend_ndl_set_media(media.backend, NULL, &pcm);
    g_fake.fail_audio_play = true;
    failures += expect_true(native_audio_feed(audio, k_pcm_block, sizeof(k_pcm_block)) == NATIVE_AUDIO_ERROR,
                            "hard AudioPlay failure is ERROR");
    g_fake.fail_audio_play = false;

    /* The caller responds to ERROR by muting, never by closing. */
    native_audio_disable(audio);
    int plays = g_fake.audio_play_calls;
    failures += expect_true(native_audio_feed(audio, k_pcm_block, sizeof(k_pcm_block)) == NATIVE_AUDIO_OK,
                            "disabled feed swallows OK");
    failures += expect_true(g_fake.audio_play_calls == plays, "disabled feed never reaches AudioPlay");

    failures += expect_true(native_audio_feed(NULL, k_pcm_block, sizeof(k_pcm_block)) == NATIVE_AUDIO_ERROR,
                            "NULL audio is ERROR");
    failures += expect_true(native_audio_feed(audio, NULL, 16) == NATIVE_AUDIO_ERROR, "NULL data is ERROR");

    native_audio_close(audio);
    backend_ndl_close(media.backend);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_open_validation();
    failures += test_feed_semantics();
    return failures ? 1 : 0;
}
