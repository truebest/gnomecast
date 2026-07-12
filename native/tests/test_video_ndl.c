#include "video_backend.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "backend_ndl_api.h"
#include "media_ndl_internal.h"

/* Exercises the gnomecast video adapter over the real backend_ndl state machine with a scripted
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
    bool has_video;
    BackendNdlVideoInfo video;
    bool has_audio;
    BackendNdlPcmInfo audio;
};

BackendNdl *native_media_ndl_backend(NativeMedia *media) {
    return media ? media->backend : NULL;
}

static BackendNdlResult apply_tracks(NativeMedia *media) {
    return backend_ndl_set_media(media->backend, media->has_video ? &media->video : NULL,
                                 media->has_audio ? &media->audio : NULL);
}

BackendNdlResult native_media_ndl_configure_video(NativeMedia *media,
                                                   const BackendNdlVideoInfo *info) {
    media->has_video = true;
    media->video = *info;
    return apply_tracks(media);
}

BackendNdlResult native_media_ndl_configure_audio(NativeMedia *media,
                                                   const BackendNdlPcmInfo *info) {
    media->has_audio = true;
    media->audio = *info;
    return apply_tracks(media);
}

BackendNdlResult native_media_ndl_clear_video(NativeMedia *media) {
    media->has_video = false;
    memset(&media->video, 0, sizeof(media->video));
    return apply_tracks(media);
}

BackendNdlResult native_media_ndl_clear_audio(NativeMedia *media) {
    media->has_audio = false;
    memset(&media->audio, 0, sizeof(media->audio));
    return apply_tracks(media);
}

typedef struct FakeNdl {
    int load_calls;
    int unload_calls;
    int video_play_calls;
    bool fail_video_play;
    NDL_DIRECTMEDIA_DATA_INFO_T last_load;
} FakeNdl;

static FakeNdl g_fake;

static void fake_reset(void) {
    memset(&g_fake, 0, sizeof(g_fake));
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
    g_fake.video_play_calls++;
    return g_fake.fail_video_play ? -1 : 0;
}
static int fake_audio_play(void *buffer, unsigned int size, long long pts) {
    (void)buffer;
    (void)size;
    (void)pts;
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
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    config.app_id = "test.video.ndl";
    config.require_keyframe_after_reload = true;
    return backend_ndl_open_with_api(&config, &api);
}

/* AVC length-prefixed AU builder (one-byte NAL payloads), same shape as the
 * test_h264_annexb vectors. */
static size_t build_avc(uint8_t *dst, const uint8_t *nal_headers, size_t nal_count) {
    size_t pos = 0;
    for (size_t i = 0; i < nal_count; i++) {
        dst[pos++] = 0;
        dst[pos++] = 0;
        dst[pos++] = 0;
        dst[pos++] = 1; /* 4-byte big-endian NAL length = 1 */
        dst[pos++] = nal_headers[i];
    }
    return pos;
}

static const uint8_t k_config_idr_nals[] = {0x67, 0x68, 0x65};
static const uint8_t k_delta_nals[] = {0x41};

static int test_open_gating_and_stream_open(void) {
    int failures = 0;
    fake_reset();
    struct NativeMedia media = {.backend = open_fake_backend()};

    NativeVideo *video = native_video_open(&media, 1920, 1080, 60);
    failures += expect_true(video != NULL, "video open");
    failures += expect_true(native_video_width(video) == 1920 && native_video_height(video) == 1080,
                            "width/height accessors");
    failures += expect_true(g_fake.load_calls == 0, "no pipeline load before first config IDR");

    uint8_t delta[16];
    size_t delta_len = build_avc(delta, k_delta_nals, 1);
    failures += expect_true(native_video_feed(video, delta, delta_len, false, 0) == NATIVE_VIDEO_NEED_KEYFRAME,
                            "delta before open needs keyframe");
    failures += expect_true(g_fake.load_calls == 0 && g_fake.video_play_calls == 0,
                            "gated delta touches nothing");

    uint8_t idr[32];
    size_t idr_len = build_avc(idr, k_config_idr_nals, 3);
    failures += expect_true(native_video_feed(video, idr, idr_len, true, 0) == NATIVE_VIDEO_OK,
                            "config IDR opens stream and feeds");
    failures += expect_true(g_fake.load_calls == 1, "stream open loads the pipeline");
    failures += expect_true(g_fake.last_load.video.type == NDL_VIDEO_TYPE_H264 &&
                            g_fake.last_load.video.width == 1920 && g_fake.last_load.video.height == 1080,
                            "load info matches open size");
    failures += expect_true(g_fake.video_play_calls == 1, "config IDR reaches VideoPlay");

    failures += expect_true(native_video_feed(video, delta, delta_len, false, 0) == NATIVE_VIDEO_OK,
                            "delta after keyframe feeds");

    /* Annex-B framing must pass through unchanged. */
    const uint8_t annexb_delta[] = {0, 0, 0, 1, 0x41};
    failures += expect_true(native_video_feed(video, annexb_delta, sizeof(annexb_delta), false, 0) == NATIVE_VIDEO_OK,
                            "Annex-B input accepted");

    native_video_close(video);
    failures += expect_true(g_fake.unload_calls == 1, "video close unloads the shared pipeline");
    backend_ndl_close(media.backend);
    return failures;
}

static int test_reload_keyframe_recovery(void) {
    int failures = 0;
    fake_reset();
    struct NativeMedia media = {.backend = open_fake_backend()};
    NativeVideo *video = native_video_open(&media, 1280, 720, 60);

    uint8_t idr[32];
    size_t idr_len = build_avc(idr, k_config_idr_nals, 3);
    uint8_t delta[16];
    size_t delta_len = build_avc(delta, k_delta_nals, 1);
    native_video_feed(video, idr, idr_len, true, 0);

    /* An audio (re)open reloads the pipeline underneath the live video track. */
    BackendNdlPcmInfo pcm = {.sample_rate_hz = 48000, .channels = 2};
    failures += expect_true(native_media_ndl_configure_audio(&media, &pcm) == BACKEND_NDL_OK,
                            "audio configure reloads");
    failures += expect_true(native_video_feed(video, delta, delta_len, false, 0) == NATIVE_VIDEO_NEED_KEYFRAME,
                            "delta after reload needs keyframe");

    /* A parameter-set-only AU must NOT clear the gate, even when the transport
     * flags it as a keyframe (grd marks SPS that way): the fresh decoder still
     * has no IDR, and letting the next P-frame report OK would kill recovery. */
    const uint8_t sps_only_nals[] = {0x67};
    uint8_t sps_only[16];
    size_t sps_only_len = build_avc(sps_only, sps_only_nals, 1);
    failures += expect_true(native_video_feed(video, sps_only, sps_only_len, true, 0) == NATIVE_VIDEO_NEED_KEYFRAME,
                            "SPS-only AU does not clear the gate");
    failures += expect_true(native_video_feed(video, delta, delta_len, false, 0) == NATIVE_VIDEO_NEED_KEYFRAME,
                            "delta after SPS-only still gated");

    failures += expect_true(native_video_feed(video, idr, idr_len, true, 0) == NATIVE_VIDEO_OK,
                            "keyframe recovers after reload");
    failures += expect_true(native_video_feed(video, delta, delta_len, false, 0) == NATIVE_VIDEO_OK,
                            "delta flows again after recovery");

    /* Removing audio reloads the surviving video track and re-arms keyframe recovery. */
    failures += expect_true(native_media_ndl_clear_audio(&media) == BACKEND_NDL_OK,
                            "audio clear reloads surviving video");
    failures += expect_true(g_fake.last_load.video.type == NDL_VIDEO_TYPE_H264 &&
                                g_fake.last_load.audio.type == 0,
                            "audio clear keeps only video");
    failures += expect_true(native_video_feed(video, delta, delta_len, false, 0) == NATIVE_VIDEO_NEED_KEYFRAME,
                            "delta after audio clear needs keyframe");
    failures += expect_true(native_video_feed(video, idr, idr_len, true, 0) == NATIVE_VIDEO_OK,
                            "keyframe recovers after audio clear");

    native_video_close(video);
    backend_ndl_close(media.backend);
    return failures;
}

static int test_terminal_errors(void) {
    int failures = 0;
    fake_reset();
    struct NativeMedia media = {.backend = open_fake_backend()};
    NativeVideo *video = native_video_open(&media, 1920, 1080, 60);

    uint8_t idr[32];
    size_t idr_len = build_avc(idr, k_config_idr_nals, 3);
    native_video_feed(video, idr, idr_len, true, 0);

    g_fake.fail_video_play = true;
    failures += expect_true(native_video_feed(video, idr, idr_len, true, 0) == NATIVE_VIDEO_ERROR,
                            "VideoPlay failure is ERROR");
    g_fake.fail_video_play = false;
    int plays = g_fake.video_play_calls;
    failures += expect_true(native_video_feed(video, idr, idr_len, true, 0) == NATIVE_VIDEO_ERROR,
                            "terminal error is sticky");
    failures += expect_true(g_fake.video_play_calls == plays, "sticky error never feeds again");
    native_video_close(video);
    backend_ndl_close(media.backend);

    fake_reset();
    media.backend = open_fake_backend();
    video = native_video_open(&media, 1920, 1080, 60);
    const uint8_t garbage[] = {'a', 'b', 'c', 'd'};
    failures += expect_true(native_video_feed(video, garbage, sizeof(garbage), false, 0) == NATIVE_VIDEO_ERROR,
                            "unparseable framing is ERROR");
    failures += expect_true(native_video_feed(video, idr, idr_len, true, 0) == NATIVE_VIDEO_ERROR,
                            "framing error is sticky");
    native_video_close(video);
    failures += expect_true(g_fake.unload_calls == 0, "never-opened stream does not unload on close");
    backend_ndl_close(media.backend);

    fake_reset();
    media.backend = open_fake_backend();
    video = native_video_open(&media, 1920, 1080, 60);
    static const uint8_t tiny[1] = {0};
    failures += expect_true(native_video_feed(video, tiny, NATIVE_VIDEO_MAX_AU_BYTES + 1, false, 0) ==
                            NATIVE_VIDEO_ERROR,
                            "oversized AU is ERROR");
    native_video_close(video);
    backend_ndl_close(media.backend);
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_open_gating_and_stream_open();
    failures += test_reload_keyframe_recovery();
    failures += test_terminal_errors();
    return failures ? 1 : 0;
}
