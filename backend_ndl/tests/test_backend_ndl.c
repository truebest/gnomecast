/* SPDX-License-Identifier: MIT */
#include "backend_ndl_api.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercises the backend_ndl state machine against a scripted fake NDL API:
 * no dlopen, no TV library. Headers come from the NDL mock include dir. */

static int expect_true(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

#define FAKE_MAX_PLAYS 32

typedef struct FakeNdl {
    int init_calls;
    int quit_calls;
    int load_calls;
    int unload_calls;
    int video_play_calls;
    int audio_play_calls;
    int dl_initialize_calls;
    int dl_finalize_calls;
    bool dl_is_initialized_value;
    bool fail_init;
    bool fail_load;
    /* One-shot Load failures: fail while >0, decrementing (fail_load stays sticky). */
    int fail_load_remaining;
    bool fail_video_play;
    bool fail_audio_play;
    int available_bytes;
    char app_id[128];
    NDL_DIRECTMEDIA_DATA_INFO_T last_load;
    long long last_video_play_pts;
    unsigned int audio_play_sizes[FAKE_MAX_PLAYS];
    long long audio_play_pts[FAKE_MAX_PLAYS];
    ResourceReleased resource_released_callback;
    NDLMediaLoadCallback media_load_callback;
} FakeNdl;

static FakeNdl g_fake;

static void fake_reset(void) {
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.available_bytes = 1 << 20;
}

static const char *fake_get_error(void) {
    return "fake error";
}

static int fake_media_init(const char *app_id, ResourceReleased cb) {
    g_fake.init_calls++;
    g_fake.resource_released_callback = cb;
    snprintf(g_fake.app_id, sizeof(g_fake.app_id), "%s", app_id ? app_id : "(null)");
    return g_fake.fail_init ? -1 : 0;
}

static int fake_media_quit(void) {
    g_fake.quit_calls++;
    return 0;
}

static int fake_media_load(NDL_DIRECTMEDIA_DATA_INFO_T *data, NDLMediaLoadCallback cb) {
    g_fake.load_calls++;
    g_fake.media_load_callback = cb;
    g_fake.last_load = *data;
    if (g_fake.fail_load_remaining > 0) {
        g_fake.fail_load_remaining--;
        return -1;
    }
    return g_fake.fail_load ? -1 : 0;
}

static int fake_media_unload(void) {
    g_fake.unload_calls++;
    return 0;
}

static int fake_video_play(void *buffer, unsigned int size, long long pts) {
    (void)buffer;
    (void)size;
    g_fake.last_video_play_pts = pts;
    g_fake.video_play_calls++;
    return g_fake.fail_video_play ? -1 : 0;
}

static int fake_audio_play(void *buffer, unsigned int size, long long pts) {
    (void)buffer;
    g_fake.audio_play_calls++;
    if (g_fake.audio_play_calls <= FAKE_MAX_PLAYS) {
        g_fake.audio_play_sizes[g_fake.audio_play_calls - 1] = size;
        g_fake.audio_play_pts[g_fake.audio_play_calls - 1] = pts;
    }
    return g_fake.fail_audio_play ? -1 : 0;
}

static bool fake_dl_initialize(void) {
    g_fake.dl_initialize_calls++;
    return true;
}

static void fake_dl_finalize(void) {
    g_fake.dl_finalize_calls++;
}

static bool fake_dl_is_initialized(void) {
    return g_fake.dl_is_initialized_value;
}

static int fake_audio_available(int *available) {
    *available = g_fake.available_bytes;
    return 0;
}

static BackendNdlApi fake_api_required_only(void) {
    BackendNdlApi api;
    memset(&api, 0, sizeof(api));
    api.DirectMediaGetError = fake_get_error;
    api.DirectMediaInit = fake_media_init;
    api.DirectMediaQuit = fake_media_quit;
    api.DirectMediaLoad = fake_media_load;
    api.DirectMediaUnload = fake_media_unload;
    api.DirectVideoPlay = fake_video_play;
    api.DirectAudioPlay = fake_audio_play;
    return api;
}

static BackendNdlApi fake_api_full(void) {
    BackendNdlApi api = fake_api_required_only();
    api.DirectMedia_DL_Initialize = fake_dl_initialize;
    api.DirectMedia_DL_Finalize = fake_dl_finalize;
    api.DirectMedia_DL_IsInitialized = fake_dl_is_initialized;
    api.DirectAudioGetAvailableBufferSize = fake_audio_available;
    return api;
}

static const BackendNdlVideoInfo k_video_1080p = {
    .codec = BACKEND_NDL_VIDEO_H264,
    .width = 1920,
    .height = 1080,
};

static const BackendNdlPcmInfo k_pcm_stereo_48k = {
    .sample_rate_hz = 48000,
    .channels = 2,
};

static const uint8_t k_au[16] = {0, 0, 0, 1, 0x65};
static const uint8_t k_pcm_block[1920] = {0};

static BackendNdlConfig test_config(void) {
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    config.app_id = "test.backend.ndl";
    config.require_keyframe_after_reload = true;
    config.prime_pcm_after_load = true;
    return config;
}

static BackendNdl *open_test_backend(const BackendNdlApi *api) {
    BackendNdlConfig config = test_config();
    return backend_ndl_open_with_api(&config, api);
}

typedef struct CallbackCapture {
    int log_calls;
    BackendNdlLogLevel last_log_level;
    char last_log[256];
    int resource_calls;
    char resource_type[64];
    int media_event_calls;
    int media_event_type;
    int64_t media_event_number;
    char media_event_text[64];
} CallbackCapture;

static void capture_log(void *userdata, BackendNdlLogLevel level, const char *message) {
    CallbackCapture *capture = (CallbackCapture *)userdata;
    capture->log_calls++;
    capture->last_log_level = level;
    snprintf(capture->last_log, sizeof(capture->last_log), "%s", message ? message : "");
}

static void capture_resource(void *userdata, const char *resource_type) {
    CallbackCapture *capture = (CallbackCapture *)userdata;
    capture->resource_calls++;
    snprintf(capture->resource_type, sizeof(capture->resource_type), "%s",
             resource_type ? resource_type : "");
}

static void capture_media_event(void *userdata, int type, int64_t number, const char *text) {
    CallbackCapture *capture = (CallbackCapture *)userdata;
    capture->media_event_calls++;
    capture->media_event_type = type;
    capture->media_event_number = number;
    snprintf(capture->media_event_text, sizeof(capture->media_event_text), "%s", text ? text : "");
}

static int test_defaults_and_callbacks(void) {
    int failures = 0;
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    failures += expect_true(config.app_id == NULL, "defaults have no application-specific id");
    failures += expect_true(config.minimum_log_level == BACKEND_NDL_LOG_INFO, "default log level is INFO");
    failures += expect_true(!config.require_keyframe_after_reload, "keyframe policy is opt-in");
    failures += expect_true(!config.prime_pcm_after_load, "PCM priming policy is opt-in");
    failures += expect_true(config.guard_audio_overflow, "overflow guard defaults on");

    fake_reset();
    CallbackCapture capture;
    memset(&capture, 0, sizeof(capture));
    BackendNdlApi api = fake_api_full();
    config.app_id = "test.callbacks.ndl";
    config.log_fn = capture_log;
    config.resource_released_fn = capture_resource;
    config.media_event_fn = capture_media_event;
    config.userdata = &capture;
    config.minimum_log_level = BACKEND_NDL_LOG_DEBUG;
    BackendNdl *ctx = backend_ndl_open_with_api(&config, &api);
    failures += expect_true(ctx != NULL && capture.log_calls > 0, "open emits callback logging");
    failures += expect_true(g_fake.resource_released_callback != NULL, "resource callback installed");
    g_fake.resource_released_callback("video");
    failures += expect_true(capture.resource_calls == 1 && strcmp(capture.resource_type, "video") == 0,
                            "resource release reaches caller");

    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, NULL) == BACKEND_NDL_OK,
                            "callback test media load");
    failures += expect_true(g_fake.media_load_callback != NULL, "media callback installed");
    g_fake.media_load_callback(0x16, 7, "ready");
    failures += expect_true(capture.media_event_calls == 1 && capture.media_event_type == 0x16 &&
                                capture.media_event_number == 7 &&
                                strcmp(capture.media_event_text, "ready") == 0,
                            "media event reaches caller");
    backend_ndl_close(ctx);

    /* Firmware callbacks racing (or trailing) close must be dropped, not
     * dereference the freed context: close() unregisters the owner first. */
    g_fake.media_load_callback(0x17, 0, "late");
    g_fake.resource_released_callback("video");
    failures += expect_true(capture.media_event_calls == 1, "post-close media event dropped");
    failures += expect_true(capture.resource_calls == 1, "post-close resource event dropped");
    return failures;
}

static int test_open_lifecycle(void) {
    int failures = 0;
    fake_reset();

    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);
    failures += expect_true(ctx != NULL, "open with defaults succeeds");
    failures += expect_true(g_fake.init_calls == 1, "MediaInit called once");
    failures += expect_true(strcmp(g_fake.app_id, "test.backend.ndl") == 0,
                            "explicit application id passed to NDL");
    failures += expect_true(g_fake.dl_initialize_calls == 1, "DL_Initialize called when not yet initialized");

    BackendNdl *second = open_test_backend(&api);
    failures += expect_true(second == NULL, "second open rejected (singleton)");

    backend_ndl_close(ctx);
    failures += expect_true(g_fake.quit_calls == 1, "MediaQuit called on close");
    failures += expect_true(g_fake.dl_finalize_calls == 1, "DL_Finalize called on close");

    ctx = open_test_backend(&api);
    failures += expect_true(ctx != NULL, "reopen after close succeeds");
    backend_ndl_close(ctx);
    return failures;
}

static int test_open_variants(void) {
    int failures = 0;

    fake_reset();
    g_fake.fail_init = true;
    BackendNdlApi api = fake_api_full();
    failures += expect_true(open_test_backend(&api) == NULL, "MediaInit failure fails open");

    fake_reset();
    g_fake.dl_is_initialized_value = true;
    BackendNdl *ctx = open_test_backend(&api);
    failures += expect_true(ctx != NULL && g_fake.dl_initialize_calls == 0,
                            "DL_Initialize skipped when already initialized");
    backend_ndl_close(ctx);
    failures += expect_true(g_fake.dl_finalize_calls == 0, "DL_Finalize skipped when we did not initialize");

    BackendNdlConfig missing_app_id;
    backend_ndl_config_defaults(&missing_app_id);
    failures += expect_true(backend_ndl_open_with_api(&missing_app_id, &api) == NULL,
                            "missing app id is rejected");

    fake_reset();
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    config.app_id = "explicit.app.id";
    ctx = backend_ndl_open_with_api(&config, &api);
    failures += expect_true(ctx != NULL && strcmp(g_fake.app_id, "explicit.app.id") == 0,
                            "explicit config app id wins");
    backend_ndl_close(ctx);

    fake_reset();
    BackendNdlApi broken = fake_api_full();
    broken.DirectVideoPlay = NULL;
    failures += expect_true(open_test_backend(&broken) == NULL,
                            "missing required entry point fails open");
    return failures;
}

static int test_load_on_first_track_and_reload(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    failures += expect_true(backend_ndl_media_time_ms(ctx) == 0, "media time 0 before first load");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_NOT_READY,
                            "video feed before load is NOT_READY");

    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, NULL) == BACKEND_NDL_OK,
                            "video-only configure loads");
    failures += expect_true(g_fake.load_calls == 1 && g_fake.unload_calls == 0, "first configure = one load");
    failures += expect_true(g_fake.last_load.video.type == NDL_VIDEO_TYPE_H264 &&
                            g_fake.last_load.video.width == 1920 && g_fake.last_load.video.height == 1080,
                            "load info carries video track");
    failures += expect_true(g_fake.last_load.audio.type == 0, "video-only load has no audio track");
    failures += expect_true(g_fake.audio_play_calls == 0, "no PCM priming for video-only load");

    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, &k_pcm_stereo_48k) == BACKEND_NDL_OK,
                            "adding audio reloads");
    failures += expect_true(g_fake.unload_calls == 1 && g_fake.load_calls == 2, "audio add = unload + load");
    failures += expect_true(g_fake.last_load.video.type == NDL_VIDEO_TYPE_H264, "reload keeps video track");
    failures += expect_true(g_fake.last_load.audio.pcm.type == NDL_AUDIO_TYPE_PCM &&
                            g_fake.last_load.audio.pcm.sampleRate == NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_48KHZ &&
                            strcmp(g_fake.last_load.audio.pcm.channelMode, "stereo") == 0 &&
                            strcmp(g_fake.last_load.audio.pcm.format, "S16LE") == 0,
                            "reload carries PCM track config");
    failures += expect_true(g_fake.audio_play_calls == 1 && g_fake.audio_play_sizes[0] == 4,
                            "PCM priming: one empty stereo frame after load");
    failures += expect_true(g_fake.audio_play_pts[0] >= 0 && g_fake.audio_play_pts[0] < 1000,
                            "priming pts is ms-since-load scale");

    failures += expect_true(backend_ndl_reload(ctx) == BACKEND_NDL_OK, "explicit reload");
    failures += expect_true(g_fake.unload_calls == 2 && g_fake.load_calls == 3, "explicit reload = unload + load");
    failures += expect_true(g_fake.audio_play_calls == 2, "priming repeats on every load with PCM");

    backend_ndl_close(ctx);
    failures += expect_true(g_fake.unload_calls == 3, "close unloads loaded media");
    return failures;
}

static int test_atomic_media_replacement(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    backend_ndl_set_media(ctx, &k_video_1080p, &k_pcm_stereo_48k);
    int loads_before = g_fake.load_calls;

    failures += expect_true(backend_ndl_set_media(ctx, NULL, &k_pcm_stereo_48k) == BACKEND_NDL_OK,
                            "atomic replacement keeps audio only");
    failures += expect_true(g_fake.load_calls == loads_before + 1,
                            "atomic replacement reloads the remaining track");
    failures += expect_true(g_fake.last_load.video.type == 0, "audio-only load has no video track");
    failures += expect_true(backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), 0) == BACKEND_NDL_OK,
                            "audio feed after replacement");

    failures += expect_true(backend_ndl_unload(ctx) == BACKEND_NDL_OK, "explicit unload ok");
    failures += expect_true(backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), 0) == BACKEND_NDL_NOT_READY,
                            "audio feed after unload is NOT_READY");

    backend_ndl_close(ctx);
    return failures;
}

static int test_keyframe_gating(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    backend_ndl_set_media(ctx, &k_video_1080p, NULL);
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, false) == BACKEND_NDL_NEED_KEYFRAME,
                            "delta after load needs keyframe");
    failures += expect_true(g_fake.video_play_calls == 0, "gated delta never reaches VideoPlay");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_OK,
                            "keyframe passes the gate");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, false) == BACKEND_NDL_OK,
                            "delta after keyframe passes");

    /* An audio (re)configure reloads the pipeline underneath live video: the
     * gate must re-arm. */
    backend_ndl_set_media(ctx, &k_video_1080p, &k_pcm_stereo_48k);
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, false) == BACKEND_NDL_NEED_KEYFRAME,
                            "gate re-arms after reload");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_OK,
                            "keyframe recovers after reload");
    backend_ndl_close(ctx);

    fake_reset();
    BackendNdlConfig config;
    backend_ndl_config_defaults(&config);
    config.app_id = "test.backend.ndl";
    config.require_keyframe_after_reload = false;
    ctx = backend_ndl_open_with_api(&config, &api);
    backend_ndl_set_media(ctx, &k_video_1080p, NULL);
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, false) == BACKEND_NDL_OK,
                            "gate disabled by config");
    backend_ndl_close(ctx);
    return failures;
}

static int test_feed_errors_and_overflow(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    backend_ndl_set_media(ctx, &k_video_1080p, &k_pcm_stereo_48k);

    failures += expect_true(
        backend_ndl_feed_video(ctx, k_au, sizeof(k_au), BACKEND_NDL_PTS_AUTO, true) == BACKEND_NDL_OK &&
            g_fake.last_video_play_pts >= 0 && g_fake.last_video_play_pts < 1000,
        "AUTO video PTS uses the current load generation");

    int auto_audio_play = g_fake.audio_play_calls;
    failures += expect_true(
        backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), BACKEND_NDL_PTS_AUTO) == BACKEND_NDL_OK &&
            g_fake.audio_play_pts[auto_audio_play] >= 0 && g_fake.audio_play_pts[auto_audio_play] < 1000,
        "AUTO audio PTS uses the current load generation");

    g_fake.fail_video_play = true;
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_ERROR,
                            "VideoPlay failure maps to ERROR");
    g_fake.fail_video_play = false;

    int audio_plays_before = g_fake.audio_play_calls;
    g_fake.available_bytes = (int)sizeof(k_pcm_block) - 1;
    failures += expect_true(backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), 0) == BACKEND_NDL_OVERFLOW,
                            "overflow guard rejects oversized chunk");
    failures += expect_true(g_fake.audio_play_calls == audio_plays_before,
                            "overflow guard skips AudioPlay");
    g_fake.available_bytes = 1 << 20;

    g_fake.fail_audio_play = true;
    failures += expect_true(backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), 0) == BACKEND_NDL_ERROR,
                            "AudioPlay failure maps to ERROR");
    g_fake.fail_audio_play = false;

    failures += expect_true(backend_ndl_feed_audio(ctx, NULL, 16, 0) == BACKEND_NDL_INVALID_ARGUMENT,
                            "NULL audio data rejected");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, 0, 0, true) == BACKEND_NDL_INVALID_ARGUMENT,
                            "empty video AU rejected");
    backend_ndl_close(ctx);

    /* Without the optional buffer-size symbol the guard must pass through. */
    fake_reset();
    g_fake.available_bytes = 0;
    BackendNdlApi minimal = fake_api_required_only();
    ctx = open_test_backend(&minimal);
    failures += expect_true(ctx != NULL, "open with required-only api");
    backend_ndl_set_media(ctx, NULL, &k_pcm_stereo_48k);
    int plays = g_fake.audio_play_calls;
    failures += expect_true(backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), 0) == BACKEND_NDL_OK,
                            "guard disabled without optional symbol");
    failures += expect_true(g_fake.audio_play_calls == plays + 1, "AudioPlay reached without guard");
    failures += expect_true(backend_ndl_get_audio_available(ctx, &plays) == BACKEND_NDL_UNAVAILABLE,
                            "optional getter reports UNAVAILABLE");
    backend_ndl_close(ctx);
    return failures;
}

static int test_load_failure_rollback(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    g_fake.fail_load = true;
    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, NULL) == BACKEND_NDL_ERROR,
                            "load failure surfaces as ERROR");
    g_fake.fail_load = false;
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_NOT_READY,
                            "failed configure leaves no video track");

    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, NULL) == BACKEND_NDL_OK,
                            "configure succeeds after failure");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_OK,
                            "video feeds after recovery");
    backend_ndl_close(ctx);
    return failures;
}

static int test_failed_reconfigure_restores_pipeline(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    backend_ndl_set_media(ctx, &k_video_1080p, NULL);
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_OK,
                            "video plays before the failed reconfigure");

    /* Adding audio unloads video+loads both; the load fails ONCE (e.g. decoder
     * reclaimed): the previous video-only pipeline must come back up. */
    g_fake.fail_load_remaining = 1;
    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, &k_pcm_stereo_48k) == BACKEND_NDL_ERROR,
                            "failed audio add surfaces as ERROR");
    failures += expect_true(g_fake.load_calls == 3, "failed load + restore load");
    failures += expect_true(g_fake.last_load.video.type == NDL_VIDEO_TYPE_H264 &&
                            g_fake.last_load.audio.type == 0,
                            "restore reloads the rolled-back video-only config");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, false) == BACKEND_NDL_NEED_KEYFRAME,
                            "gate re-armed by the restore load");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_OK,
                            "video recovers after the restore");
    failures += expect_true(backend_ndl_feed_audio(ctx, k_pcm_block, sizeof(k_pcm_block), 0) ==
                            BACKEND_NDL_NOT_READY,
                            "rolled-back audio track stays unconfigured");

    /* Persistent load failure: restore cannot help; pipeline stays down until an
     * explicit reload succeeds. */
    g_fake.fail_load = true;
    failures += expect_true(backend_ndl_set_media(ctx, &k_video_1080p, &k_pcm_stereo_48k) == BACKEND_NDL_ERROR,
                            "persistent failure still ERROR");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_NOT_READY,
                            "pipeline down when restore also fails");
    g_fake.fail_load = false;
    failures += expect_true(backend_ndl_reload(ctx) == BACKEND_NDL_OK, "explicit reload recovers");
    failures += expect_true(backend_ndl_feed_video(ctx, k_au, sizeof(k_au), 0, true) == BACKEND_NDL_OK,
                            "video plays after explicit reload");

    backend_ndl_close(ctx);
    return failures;
}

static int test_pcm_validation(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    BackendNdlPcmInfo bad_rate = {.sample_rate_hz = 11025, .channels = 2};
    BackendNdlPcmInfo bad_channels = {.sample_rate_hz = 48000, .channels = 3};
    BackendNdlPcmInfo mono = {.sample_rate_hz = 48000, .channels = 1};

    failures += expect_true(backend_ndl_set_media(ctx, NULL, &bad_rate) == BACKEND_NDL_UNSUPPORTED,
                            "unsupported sample rate rejected");
    failures += expect_true(backend_ndl_set_media(ctx, NULL, &bad_channels) == BACKEND_NDL_UNSUPPORTED,
                            "unsupported channel count rejected");
    failures += expect_true(g_fake.load_calls == 0, "rejected configs never load");

    failures += expect_true(backend_ndl_set_media(ctx, NULL, &mono) == BACKEND_NDL_OK, "mono accepted");
    failures += expect_true(strcmp(g_fake.last_load.audio.pcm.channelMode, "mono") == 0, "mono channel mode string");
    failures += expect_true(g_fake.audio_play_sizes[g_fake.audio_play_calls - 1] == 2,
                            "mono priming frame is 2 bytes");

    failures += expect_true(backend_ndl_pcm_sample_rate_supported(48000), "48kHz supported");
    failures += expect_true(!backend_ndl_pcm_sample_rate_supported(11025), "11.025kHz unsupported");
    backend_ndl_close(ctx);
    return failures;
}

static int test_video_validation(void) {
    int failures = 0;
    fake_reset();
    BackendNdlApi api = fake_api_full();
    BackendNdl *ctx = open_test_backend(&api);

    BackendNdlVideoInfo zero_width = k_video_1080p;
    BackendNdlVideoInfo oversized_width = k_video_1080p;
    BackendNdlVideoInfo oversized_height = k_video_1080p;
    zero_width.width = 0;
    oversized_width.width = UINT32_MAX;
    oversized_height.height = UINT32_MAX;

    failures += expect_true(backend_ndl_set_media(ctx, &zero_width, NULL) == BACKEND_NDL_INVALID_ARGUMENT,
                            "zero video width rejected");
    failures += expect_true(backend_ndl_set_media(ctx, &oversized_width, NULL) == BACKEND_NDL_INVALID_ARGUMENT,
                            "video width outside NDL int range rejected");
    failures += expect_true(backend_ndl_set_media(ctx, &oversized_height, NULL) == BACKEND_NDL_INVALID_ARGUMENT,
                            "video height outside NDL int range rejected");
    failures += expect_true(g_fake.load_calls == 0, "invalid video sizes never load");

    backend_ndl_close(ctx);
    return failures;
}

static int test_result_names(void) {
    int failures = 0;
    failures += expect_true(strcmp(backend_ndl_result_name(BACKEND_NDL_OK), "OK") == 0, "result name OK");
    failures += expect_true(strcmp(backend_ndl_result_name(BACKEND_NDL_NEED_KEYFRAME), "NEED_KEYFRAME") == 0,
                            "result name NEED_KEYFRAME");
    failures += expect_true(strcmp(backend_ndl_result_name((BackendNdlResult)99), "UNKNOWN") == 0,
                            "result name UNKNOWN");
    return failures;
}

int main(void) {
    int failures = 0;
    failures += test_defaults_and_callbacks();
    failures += test_open_lifecycle();
    failures += test_open_variants();
    failures += test_load_on_first_track_and_reload();
    failures += test_atomic_media_replacement();
    failures += test_keyframe_gating();
    failures += test_feed_errors_and_overflow();
    failures += test_load_failure_rollback();
    failures += test_failed_reconfigure_restores_pipeline();
    failures += test_pcm_validation();
    failures += test_video_validation();
    failures += test_result_names();
    return failures ? 1 : 0;
}
