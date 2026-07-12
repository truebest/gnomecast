/* SPDX-License-Identifier: MIT */
#ifndef BACKEND_NDL_TEST_DOUBLE_NDL_DIRECTMEDIA_H
#define BACKEND_NDL_TEST_DOUBLE_NDL_DIRECTMEDIA_H

/*
 * Host-test double for the webOS NDL DirectMedia SDK header.
 *
 * Authored for this repository. It records only the ABI facts backend_ndl and
 * its tests consume — struct layouts, enum values, and the strings the
 * firmware matches — nothing else. It is not a copy of any SDK header: real
 * builds compile against the NDK sysroot via pkg-config, and this file exists
 * solely so the state machine compiles on hosts without the NDK.
 *
 * Every fact here was verified against the webOS NDK sysroot header set (and
 * layout-checked with the cross compiler; see the README). Do not extend it
 * from memory: verify any addition against the sysroot first, and against the
 * device when behavior is involved. The SDK's HDR structures are deliberately
 * not modeled (backend_ndl does not use the HDR API, and the real ABI there is
 * known to vary between header sets).
 */

#if defined(NDL_DIRECTMEDIA_API_VERSION) && (NDL_DIRECTMEDIA_API_VERSION != 2)
#error "this test double models DirectMedia API version 2 only"
#endif
#ifndef NDL_DIRECTMEDIA_API_VERSION
#define NDL_DIRECTMEDIA_API_VERSION 2
#endif

#include <stdbool.h>

/* Firmware notification that a previously granted media resource was revoked
 * (e.g. another application took the decoder). */
typedef void (*ResourceReleased)(const char *type);

/* Asynchronous media state events emitted after a media load; the observed
 * STATE_UPDATE codes include 0x16 LOADCOMPLETED, 0x17 UNLOADCOMPLETED and
 * 0x1a PLAYING. */
typedef void (*NDLMediaLoadCallback)(int type, long long value, const char *text);

typedef enum BackendNdlTestAppState {
    NDL_DIRECTMEDIA_APP_STATE_FOREGROUND = 0,
    NDL_DIRECTMEDIA_APP_STATE_BACKGROUND = 1
} NDL_DIRECTMEDIA_APP_STATE;

typedef enum BackendNdlTestVideoType {
    NDL_VIDEO_TYPE_H264 = 1,
    NDL_VIDEO_TYPE_H265 = 2,
    NDL_VIDEO_TYPE_VP9 = 3,
    NDL_VIDEO_TYPE_AV1 = 4
} NDL_VIDEO_TYPE;

typedef enum BackendNdlTestAudioType {
    NDL_AUDIO_TYPE_PCM = 1,
    NDL_AUDIO_TYPE_MP3 = 2,
    NDL_AUDIO_TYPE_OPUS = 3
} NDL_AUDIO_TYPE;

/* PCM sample rates are an enum table, NOT hertz values. */
typedef enum BackendNdlTestPcmSampleRate {
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_NONE = 0,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_48KHZ = 1,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_44KHZ = 2,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_32KHZ = 3,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_24KHZ = 4,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_22KHZ = 5,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_16KHZ = 6,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_12KHZ = 7,
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_8KHZ = 8
} NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE;

/* Strings the firmware matches by content. */
#define NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE "S16LE"
#define NDL_DIRECTMEDIA_AUDIO_PCM_MODE_STEREO "stereo"

static inline NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(int hertz) {
    switch (hertz) {
        case 48000:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_48KHZ;
        case 44100:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_44KHZ;
        case 32000:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_32KHZ;
        case 24000:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_24KHZ;
        case 22050:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_22KHZ;
        case 16000:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_16KHZ;
        case 12000:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_12KHZ;
        case 8000:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_8KHZ;
        default:
            return NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_NONE;
    }
}

typedef struct BackendNdlTestPcmInfo {
    NDL_AUDIO_TYPE type;
    int unknown1;            /* zeroed by every known user */
    const char *format;      /* NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_* */
    const char *layout;      /* the firmware accepts NULL for interleaved S16LE */
    const char *channelMode; /* "mono" / "stereo" / "6-channel" */
    NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE sampleRate;
} NDL_DIRECTAUDIO_PCM_INFO_T;

/* Unused by backend_ndl (no Opus passthrough) but part of the audio union's
 * layout in the real ABI. sampleRate units are undocumented by the SDK. */
typedef struct BackendNdlTestOpusInfo {
    NDL_AUDIO_TYPE type;
    int unknown1;
    int channels;
    int unknown2;
    double sampleRate;
    const char *streamHeader;
} NDL_DIRECTMEDIA_AUDIO_OPUS_INFO_T;

/* One combined pipeline configuration: a zeroed member means "track absent". */
typedef struct BackendNdlTestDataInfo {
    struct BackendNdlTestVideoInfo {
        int width;
        int height;
        NDL_VIDEO_TYPE type;
        int unknown1;
    } video;
    union {
        NDL_AUDIO_TYPE type;
        NDL_DIRECTAUDIO_PCM_INFO_T pcm;
        NDL_DIRECTMEDIA_AUDIO_OPUS_INFO_T opus;
        char padding[32];
    } audio;
} NDL_DIRECTMEDIA_DATA_INFO_T;

#endif
