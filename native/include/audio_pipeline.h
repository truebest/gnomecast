#ifndef GNOMECAST_AUDIO_PIPELINE_H
#define GNOMECAST_AUDIO_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The application-wide audio graph has one immutable sink format. Each RDP source may
 * use its own input rate; source-local miniaudio converters feed the float graph. */
#define NATIVE_AUDIO_PIPELINE_MAX_SOURCES 4
#define NATIVE_AUDIO_PIPELINE_SAMPLE_RATE 48000u
#define NATIVE_AUDIO_PIPELINE_CHANNELS 2u
#define NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES 480u
/* Power-of-two so 32-bit SPSC cursor wrap remains physically contiguous; ~1.37 seconds. */
#define NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES 65536u

#define NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15 32768
#define NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15 65536

typedef uint64_t (*NativeAudioPipelineClock)(void *ctx);

typedef struct NativeAudioSourceStats {
    uint32_t queue_ms;
    uint32_t target_delay_ms;
    uint32_t jitter_p95_ms;
    int32_t src_correction_ppm;
    uint64_t underruns;
    uint64_t hard_corrections;
    uint64_t overflows;
} NativeAudioSourceStats;

/* Kept opaque so miniaudio configuration and types do not leak into the application ABI. */
typedef struct NativeAudioPipeline {
    void *impl;
} NativeAudioPipeline;

/* init_with_clock is for deterministic controller tests. Passing NULL uses
 * CLOCK_MONOTONIC. All allocation and graph wiring happens during initialization. */
bool native_audio_pipeline_init(NativeAudioPipeline *pipeline);
bool native_audio_pipeline_init_with_clock(NativeAudioPipeline *pipeline, NativeAudioPipelineClock clock,
                                           void *clock_ctx);
void native_audio_pipeline_destroy(NativeAudioPipeline *pipeline);
bool native_audio_pipeline_is_initialized(const NativeAudioPipeline *pipeline);

/* Configures and opens one SPSC source. A format change is handed to the render side by
 * an atomic generation boundary; queued frames from the previous generation are dropped.
 * Mono and stereo S16 input are accepted. Gain survives close/reopen. */
bool native_audio_pipeline_set_source_format(NativeAudioPipeline *pipeline, int source, uint32_t sample_rate,
                                             uint16_t channels);
void native_audio_pipeline_close_source(NativeAudioPipeline *pipeline, int source);
void native_audio_pipeline_set_source_gain(NativeAudioPipeline *pipeline, int source, int32_t gain_q15);

/* Producer path. The source's decoded interleaved S16 frames are copied synchronously.
 * Returns the number of new frames rejected at the emergency capacity limit. The
 * producer never moves the consumer cursor: overflow only requests a consumer-side trim. */
size_t native_audio_pipeline_push(NativeAudioPipeline *pipeline, int source, const int16_t *samples, size_t frames,
                                  uint32_t timestamp_ms);

/* Render path used by the NDL pump. These functions perform no allocation, logging,
 * blocking lock, or wait. Exactly one sink may render at a time. read_f32 exposes the
 * graph's native format; read_s16 applies the NDL boundary conversion. */
bool native_audio_pipeline_read_f32(NativeAudioPipeline *pipeline, float *out, size_t frames);
bool native_audio_pipeline_read_s16(NativeAudioPipeline *pipeline, int16_t *out, size_t frames);

void native_audio_pipeline_get_source_peaks(NativeAudioPipeline *pipeline, int source, int32_t *left,
                                            int32_t *right);
void native_audio_pipeline_get_output_peaks(NativeAudioPipeline *pipeline, int32_t *left, int32_t *right);
bool native_audio_pipeline_get_source_stats(NativeAudioPipeline *pipeline, int source,
                                            NativeAudioSourceStats *stats);

/* NDL sink scheduler. The feed callback runs without a pipeline lock. */
bool native_audio_pipeline_pump_start(NativeAudioPipeline *pipeline,
                                      void (*feed)(void *ctx, const int16_t *samples, size_t frames), void *feed_ctx);
void native_audio_pipeline_pump_stop(NativeAudioPipeline *pipeline);

#if defined(HELLOLG_AUDIO_PIPELINE_TESTING)
typedef void (*NativeAudioPipelineTestHook)(void *ctx, int source);
void native_audio_pipeline_set_test_before_ring_read(NativeAudioPipeline *pipeline,
                                                     NativeAudioPipelineTestHook hook, void *ctx);
#endif

#endif
