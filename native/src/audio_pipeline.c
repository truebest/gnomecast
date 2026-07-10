#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "audio_pipeline.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Keep this translation unit and the separately compiled upstream miniaudio.c on the
 * same headless feature set. */
#ifndef MA_NO_DEVICE_IO
#define MA_NO_DEVICE_IO
#endif
#ifndef MA_NO_DECODING
#define MA_NO_DECODING
#endif
#ifndef MA_NO_ENCODING
#define MA_NO_ENCODING
#endif
#ifndef MA_NO_RESOURCE_MANAGER
#define MA_NO_RESOURCE_MANAGER
#endif
#ifndef MA_NO_GENERATION
#define MA_NO_GENERATION
#endif
#ifndef MA_NO_ENGINE
#define MA_NO_ENGINE
#endif
#ifndef MA_NO_THREADING
#define MA_NO_THREADING
#endif
#ifndef MA_NO_RUNTIME_LINKING
#define MA_NO_RUNTIME_LINKING
#endif
#include "miniaudio.h"

_Static_assert(MA_VERSION_MAJOR == 0 && MA_VERSION_MINOR == 11 && MA_VERSION_REVISION == 25,
               "gnomecast audio pipeline is pinned to miniaudio 0.11.25");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "the audio SPSC cursors require lock-free 32-bit atomics");
_Static_assert((NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES & (NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - 1u)) == 0,
               "the SPSC capacity must preserve ring positions across cursor wrap");

#define NATIVE_AUDIO_TARGET_INITIAL_MS 60u
#define NATIVE_AUDIO_TARGET_MIN_MS 40u
#define NATIVE_AUDIO_TARGET_MAX_MS 150u
#define NATIVE_AUDIO_TARGET_HEADROOM_MS 20u
#define NATIVE_AUDIO_TARGET_DECAY_MS 5000u
#define NATIVE_AUDIO_TARGET_DECAY_STEP_MS 10u
#define NATIVE_AUDIO_LONG_GAP_MS 500u
#define NATIVE_AUDIO_STALE_BURST_MS 100u
#define NATIVE_AUDIO_HARD_TRIM_ERROR_MS 80u
#define NATIVE_AUDIO_HARD_TRIM_KEEP_MS 20u
#define NATIVE_AUDIO_FADE_FRAMES 240u /* 5 ms at the fixed 48 kHz graph rate. */
#define NATIVE_AUDIO_MAX_CORRECTION_PPM 5000
#define NATIVE_AUDIO_CORRECTION_STEP_PPM 250
#define NATIVE_AUDIO_JITTER_WINDOW_MS 10000u
#define NATIVE_AUDIO_JITTER_BUCKET_MS 5u
#define NATIVE_AUDIO_JITTER_BUCKETS 31u
#define NATIVE_AUDIO_JITTER_SAMPLES 2048u
#define NATIVE_AUDIO_STATS_LOG_MS 3000u

typedef struct NativeJitterSample {
    uint64_t arrival_ms;
    uint8_t bucket;
} NativeJitterSample;

typedef struct NativeAudioPipelineImpl NativeAudioPipelineImpl;

typedef struct NativeAudioSource {
    /* Required first member for a custom miniaudio data source. */
    ma_data_source_base base;
    bool base_initialized;
    ma_data_source_node node;
    bool node_initialized;
    NativeAudioPipelineImpl *pipeline;
    int index;

    /* Fixed stereo storage avoids changing the physical ring stride across a format
     * generation. Mono input is duplicated by the producer before publication. */
    int16_t *ring;
    void *converter_heap;
    ma_data_converter converter;
    bool converter_initialized;

    atomic_uint read_cursor;
    atomic_uint write_cursor;
    atomic_uint reset_cursor;
    atomic_uint format_generation;
    atomic_uint pending_sample_rate;
    atomic_uint pending_channels;
    atomic_bool open;
    atomic_bool trim_requested;
    atomic_uint talkspurt_cursor;
    atomic_uint talkspurt_generation;
    atomic_uint last_push_ms;

    atomic_int gain_q15;
    atomic_uint target_delay_ms;
    atomic_uint jitter_p95_ms;
    atomic_int correction_ppm;
    atomic_uint underruns;
    atomic_uint hard_corrections;
    atomic_uint overflows;
    atomic_uint peak_left;
    atomic_uint peak_right;
    atomic_uint peak_when_ms;

    /* Producer-owned controller state (one RDP worker per source). */
    uint32_t producer_sample_rate;
    uint16_t producer_channels;
    bool have_arrival;
    uint64_t last_arrival_ms;
    uint32_t last_timestamp_ms;
    uint64_t last_target_change_ms;
    unsigned seen_underruns;
    NativeJitterSample jitter_samples[NATIVE_AUDIO_JITTER_SAMPLES];
    uint16_t jitter_histogram[NATIVE_AUDIO_JITTER_BUCKETS];
    unsigned jitter_head;
    unsigned jitter_count;

    /* Consumer-owned converter/playout state. */
    unsigned applied_generation;
    unsigned seen_talkspurt_generation;
    uint32_t sample_rate;
    uint16_t channels;
    bool live;
    bool rebuffering;
    bool trim_after_fade;
    unsigned fade_out_remaining;
    unsigned fade_in_remaining;
    int current_correction_ppm;
    int drift_integral_ppm;
    float current_gain;
} NativeAudioSource;

struct NativeAudioPipelineImpl {
    ma_node_graph graph;
    bool graph_initialized;
    NativeAudioSource sources[NATIVE_AUDIO_PIPELINE_MAX_SOURCES];
    NativeAudioPipelineClock clock;
    void *clock_ctx;

    atomic_uint output_peak_left;
    atomic_uint output_peak_right;
    atomic_uint output_peak_when_ms;

    float conversion_buffer[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * NATIVE_AUDIO_PIPELINE_CHANNELS];
    int16_t pump_buffer[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * NATIVE_AUDIO_PIPELINE_CHANNELS];

    pthread_mutex_t control_lock;
    bool control_lock_initialized;
    pthread_t pump_thread;
    bool pump_running;
    atomic_bool pump_stop;
    void (*feed)(void *ctx, const int16_t *samples, size_t frames);
    void *feed_ctx;
    uint64_t last_stats_log_ms;

#if defined(HELLOLG_AUDIO_PIPELINE_TESTING)
    NativeAudioPipelineTestHook before_ring_read;
    void *before_ring_read_ctx;
#endif
};

static uint64_t native_audio_monotonic_ms(void *ctx) {
    (void)ctx;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t pipeline_now_ms(const NativeAudioPipelineImpl *pipeline) {
    return pipeline->clock(pipeline->clock_ctx);
}

static bool source_index_valid(int source) {
    return source >= 0 && source < NATIVE_AUDIO_PIPELINE_MAX_SOURCES;
}

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static unsigned source_available_frames(const NativeAudioSource *source) {
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_acquire);
    return write - read;
}

static uint32_t source_queue_ms(const NativeAudioSource *source) {
    uint32_t rate = atomic_load_explicit(&source->pending_sample_rate, memory_order_relaxed);
    return rate ? (uint32_t)((uint64_t)source_available_frames(source) * 1000u / rate) : 0;
}

static void source_jitter_clear(NativeAudioSource *source) {
    memset(source->jitter_histogram, 0, sizeof(source->jitter_histogram));
    source->jitter_head = 0;
    source->jitter_count = 0;
    atomic_store_explicit(&source->jitter_p95_ms, 0u, memory_order_relaxed);
}

static void source_jitter_remove_oldest(NativeAudioSource *source) {
    if (source->jitter_count == 0) {
        return;
    }
    NativeJitterSample *sample = &source->jitter_samples[source->jitter_head];
    if (sample->bucket < NATIVE_AUDIO_JITTER_BUCKETS && source->jitter_histogram[sample->bucket] > 0) {
        source->jitter_histogram[sample->bucket]--;
    }
    source->jitter_head = (source->jitter_head + 1u) % NATIVE_AUDIO_JITTER_SAMPLES;
    source->jitter_count--;
}

static void source_jitter_expire(NativeAudioSource *source, uint64_t now_ms) {
    while (source->jitter_count > 0) {
        const NativeJitterSample *sample = &source->jitter_samples[source->jitter_head];
        if (now_ms >= sample->arrival_ms && now_ms - sample->arrival_ms <= NATIVE_AUDIO_JITTER_WINDOW_MS) {
            break;
        }
        source_jitter_remove_oldest(source);
    }
}

static uint32_t source_jitter_p95(const NativeAudioSource *source) {
    if (source->jitter_count == 0) {
        return 0;
    }
    unsigned threshold = (source->jitter_count * 95u + 99u) / 100u;
    unsigned seen = 0;
    for (unsigned bucket = 0; bucket < NATIVE_AUDIO_JITTER_BUCKETS; bucket++) {
        seen += source->jitter_histogram[bucket];
        if (seen >= threshold) {
            return bucket * NATIVE_AUDIO_JITTER_BUCKET_MS;
        }
    }
    return (NATIVE_AUDIO_JITTER_BUCKETS - 1u) * NATIVE_AUDIO_JITTER_BUCKET_MS;
}

static void source_jitter_add(NativeAudioSource *source, uint64_t now_ms, uint32_t variation_ms) {
    source_jitter_expire(source, now_ms);
    if (source->jitter_count == NATIVE_AUDIO_JITTER_SAMPLES) {
        source_jitter_remove_oldest(source);
    }
    unsigned bucket = variation_ms / NATIVE_AUDIO_JITTER_BUCKET_MS;
    if (bucket >= NATIVE_AUDIO_JITTER_BUCKETS) {
        bucket = NATIVE_AUDIO_JITTER_BUCKETS - 1u;
    }
    unsigned tail = (source->jitter_head + source->jitter_count) % NATIVE_AUDIO_JITTER_SAMPLES;
    source->jitter_samples[tail].arrival_ms = now_ms;
    source->jitter_samples[tail].bucket = (uint8_t)bucket;
    source->jitter_histogram[bucket]++;
    source->jitter_count++;
    atomic_store_explicit(&source->jitter_p95_ms, source_jitter_p95(source), memory_order_relaxed);
}

static void source_raise_target(NativeAudioSource *source, uint32_t desired_ms) {
    desired_ms = clamp_u32(desired_ms, NATIVE_AUDIO_TARGET_MIN_MS, NATIVE_AUDIO_TARGET_MAX_MS);
    unsigned current = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    while (current < desired_ms &&
           !atomic_compare_exchange_weak_explicit(&source->target_delay_ms, &current, desired_ms,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void source_update_arrival_controller(NativeAudioSource *source, size_t frames, uint32_t timestamp_ms,
                                             uint64_t now_ms, unsigned write_boundary) {
    uint32_t duration_ms = source->producer_sample_rate
                               ? (uint32_t)(((uint64_t)frames * 1000u + source->producer_sample_rate / 2u) /
                                            source->producer_sample_rate)
                               : 0;
    if (duration_ms == 0) {
        duration_ms = 1;
    }

    if (!source->have_arrival || now_ms < source->last_arrival_ms) {
        source->have_arrival = true;
        source->last_arrival_ms = now_ms;
        source->last_timestamp_ms = timestamp_ms;
        source->last_target_change_ms = now_ms;
        return;
    }

    uint64_t arrival_delta_64 = now_ms - source->last_arrival_ms;
    uint32_t arrival_delta = arrival_delta_64 > UINT32_MAX ? UINT32_MAX : (uint32_t)arrival_delta_64;
    if (arrival_delta > NATIVE_AUDIO_LONG_GAP_MS) {
        /* Silence/talkspurt boundary, not a jitter observation. */
        source_jitter_clear(source);
        atomic_store_explicit(&source->target_delay_ms, NATIVE_AUDIO_TARGET_INITIAL_MS, memory_order_relaxed);
        atomic_store_explicit(&source->talkspurt_cursor, write_boundary, memory_order_release);
        atomic_fetch_add_explicit(&source->talkspurt_generation, 1u, memory_order_release);
        source->last_target_change_ms = now_ms;
        source->last_arrival_ms = now_ms;
        source->last_timestamp_ms = timestamp_ms;
        return;
    }

    uint32_t expected_ms = duration_ms;
    if (timestamp_ms != 0 && source->last_timestamp_ms != 0) {
        uint32_t timestamp_delta = timestamp_ms - source->last_timestamp_ms; /* wrap is intentional */
        if (timestamp_delta != 0 && timestamp_delta <= NATIVE_AUDIO_LONG_GAP_MS) {
            expected_ms = timestamp_delta;
        }
    }
    uint32_t variation_ms = arrival_delta > expected_ms ? arrival_delta - expected_ms : expected_ms - arrival_delta;
    source_jitter_add(source, now_ms, variation_ms);

    unsigned underruns = atomic_load_explicit(&source->underruns, memory_order_relaxed);
    if (underruns != source->seen_underruns) {
        source->seen_underruns = underruns;
        source->last_target_change_ms = now_ms;
    }

    uint32_t peak_target = clamp_u32(variation_ms + NATIVE_AUDIO_TARGET_HEADROOM_MS,
                                     NATIVE_AUDIO_TARGET_MIN_MS, NATIVE_AUDIO_TARGET_MAX_MS);
    unsigned target = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    if (peak_target > target) {
        source_raise_target(source, peak_target);
        source->last_target_change_ms = now_ms;
    } else {
        uint32_t p95 = atomic_load_explicit(&source->jitter_p95_ms, memory_order_relaxed);
        uint32_t calculated = clamp_u32(p95 + NATIVE_AUDIO_TARGET_HEADROOM_MS,
                                        NATIVE_AUDIO_TARGET_MIN_MS, NATIVE_AUDIO_TARGET_MAX_MS);
        target = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
        if (calculated < target && now_ms - source->last_target_change_ms >= NATIVE_AUDIO_TARGET_DECAY_MS) {
            unsigned reduced = target > NATIVE_AUDIO_TARGET_DECAY_STEP_MS
                                   ? target - NATIVE_AUDIO_TARGET_DECAY_STEP_MS
                                   : NATIVE_AUDIO_TARGET_MIN_MS;
            if (reduced < calculated) {
                reduced = calculated;
            }
            (void)atomic_compare_exchange_strong_explicit(&source->target_delay_ms, &target, reduced,
                                                          memory_order_relaxed, memory_order_relaxed);
            source->last_target_change_ms = now_ms;
        }
    }

    source->last_arrival_ms = now_ms;
    source->last_timestamp_ms = timestamp_ms;
}

static ma_data_converter_config source_converter_config(uint32_t sample_rate) {
    ma_data_converter_config config =
        ma_data_converter_config_init(ma_format_s16, ma_format_f32, NATIVE_AUDIO_PIPELINE_CHANNELS,
                                      NATIVE_AUDIO_PIPELINE_CHANNELS, sample_rate,
                                      NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
    config.allowDynamicSampleRate = MA_TRUE;
    config.resampling.algorithm = ma_resample_algorithm_linear;
    config.resampling.linear.lpfOrder = 1;
    return config;
}

static bool source_reset_converter(NativeAudioSource *source) {
    if (source->converter_initialized) {
        ma_data_converter_uninit(&source->converter, NULL);
        source->converter_initialized = false;
    }
    ma_data_converter_config config = source_converter_config(source->sample_rate);
    if (ma_data_converter_init_preallocated(&config, source->converter_heap, &source->converter) != MA_SUCCESS) {
        return false;
    }
    source->converter_initialized = true;
    return true;
}

static void source_advance_read_cursor(NativeAudioSource *source, unsigned boundary) {
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_relaxed);
    unsigned advance = boundary - read;
    if (advance <= NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES) {
        atomic_store_explicit(&source->read_cursor, boundary, memory_order_release);
    }
}

static bool source_apply_generation(NativeAudioSource *source) {
    unsigned generation = atomic_load_explicit(&source->format_generation, memory_order_acquire);
    if (generation == source->applied_generation) {
        return false;
    }
    source->sample_rate = atomic_load_explicit(&source->pending_sample_rate, memory_order_relaxed);
    source->channels = (uint16_t)atomic_load_explicit(&source->pending_channels, memory_order_relaxed);
    unsigned boundary = atomic_load_explicit(&source->reset_cursor, memory_order_acquire);
    source_advance_read_cursor(source, boundary);
    source->applied_generation = generation;
    source->live = false;
    source->rebuffering = true;
    source->trim_after_fade = false;
    source->fade_out_remaining = 0;
    source->fade_in_remaining = 0;
    source->current_correction_ppm = 0;
    source->drift_integral_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    (void)source_reset_converter(source);
    return true;
}

static bool source_apply_talkspurt(NativeAudioSource *source) {
    unsigned generation = atomic_load_explicit(&source->talkspurt_generation, memory_order_acquire);
    if (generation == source->seen_talkspurt_generation) {
        return false;
    }
    unsigned boundary = atomic_load_explicit(&source->talkspurt_cursor, memory_order_acquire);
    source_advance_read_cursor(source, boundary);
    source->seen_talkspurt_generation = generation;
    source->live = false;
    source->rebuffering = true;
    source->trim_after_fade = false;
    source->fade_out_remaining = 0;
    source->fade_in_remaining = 0;
    source->current_correction_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    if (source->converter_initialized) {
        (void)ma_data_converter_reset(&source->converter);
    }
    return true;
}

static size_t source_trim_to_target(NativeAudioSource *source) {
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_relaxed);
    unsigned available = write - read;
    uint32_t target_ms = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    uint64_t keep_frames_64 = (uint64_t)(target_ms + NATIVE_AUDIO_HARD_TRIM_KEEP_MS) * source->sample_rate / 1000u;
    unsigned keep_frames = keep_frames_64 > UINT_MAX ? UINT_MAX : (unsigned)keep_frames_64;
    if (available <= keep_frames) {
        atomic_store_explicit(&source->trim_requested, false, memory_order_relaxed);
        return 0;
    }
    unsigned dropped = available - keep_frames;
    atomic_store_explicit(&source->read_cursor, read + dropped, memory_order_release);
    if (source->converter_initialized) {
        (void)ma_data_converter_reset(&source->converter);
    }
    source->current_correction_ppm = 0;
    atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
    atomic_store_explicit(&source->trim_requested, false, memory_order_relaxed);
    atomic_fetch_add_explicit(&source->hard_corrections, 1u, memory_order_relaxed);
    return dropped;
}

static void source_update_rate_correction(NativeAudioSource *source) {
    if (!source->converter_initialized || source->sample_rate == 0 || !source->live) {
        source->current_correction_ppm = 0;
        atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
        return;
    }
    /* Control the post-read queue: the current graph block is about to consume 10 ms,
     * so its frames sit on top of the desired standing target at this instant. */
    int error_ms = (int)source_queue_ms(source) -
                   (int)atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed) -
                   (int)(NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 1000u / NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
    int proportional = 0;
    int magnitude = error_ms < 0 ? -error_ms : error_ms;
    if (magnitude > 10) {
        if (magnitude > 50) {
            magnitude = 50;
        }
        proportional = magnitude * NATIVE_AUDIO_MAX_CORRECTION_PPM / 50;
        if (error_ms < 0) {
            proportional = -proportional;
        }
    }
    int integral_step = error_ms / 4;
    if (integral_step == 0 && error_ms != 0) {
        integral_step = error_ms < 0 ? -1 : 1;
    }
    if (integral_step != 0) {
        source->drift_integral_ppm += integral_step;
        if (source->drift_integral_ppm > NATIVE_AUDIO_MAX_CORRECTION_PPM) {
            source->drift_integral_ppm = NATIVE_AUDIO_MAX_CORRECTION_PPM;
        } else if (source->drift_integral_ppm < -NATIVE_AUDIO_MAX_CORRECTION_PPM) {
            source->drift_integral_ppm = -NATIVE_AUDIO_MAX_CORRECTION_PPM;
        }
    }
    int desired = proportional + source->drift_integral_ppm;
    if (desired > NATIVE_AUDIO_MAX_CORRECTION_PPM) {
        desired = NATIVE_AUDIO_MAX_CORRECTION_PPM;
    } else if (desired < -NATIVE_AUDIO_MAX_CORRECTION_PPM) {
        desired = -NATIVE_AUDIO_MAX_CORRECTION_PPM;
    }
    if (desired > source->current_correction_ppm + NATIVE_AUDIO_CORRECTION_STEP_PPM) {
        source->current_correction_ppm += NATIVE_AUDIO_CORRECTION_STEP_PPM;
    } else if (desired < source->current_correction_ppm - NATIVE_AUDIO_CORRECTION_STEP_PPM) {
        source->current_correction_ppm -= NATIVE_AUDIO_CORRECTION_STEP_PPM;
    } else {
        source->current_correction_ppm = desired;
    }
    float ratio = (float)source->sample_rate / (float)NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
    ratio *= 1.0f + (float)source->current_correction_ppm / 1000000.0f;
    (void)ma_data_converter_set_rate_ratio(&source->converter, ratio);
    atomic_store_explicit(&source->correction_ppm, source->current_correction_ppm, memory_order_relaxed);
}

static size_t source_convert_frames(NativeAudioSource *source, float *out, size_t frames,
                                    bool *discontinuity_applied) {
    size_t produced = 0;
    *discontinuity_applied = false;
    while (produced < frames && source->converter_initialized) {
        /* The producer publishes a format/talkspurt generation before it publishes
         * the corresponding new PCM through write_cursor. Loading the cursor first
         * and rechecking both generations afterwards means that either this snapshot
         * contains only the old generation, or the discontinuity is applied before
         * any newly published ring frames are consumed. */
        unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
        bool format_changed = source_apply_generation(source);
        bool talkspurt_changed = source_apply_talkspurt(source);
        if (format_changed || talkspurt_changed) {
            *discontinuity_applied = true;
            break;
        }
        unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_relaxed);
        unsigned available = write - read;
        if (available == 0) {
            break;
        }
        unsigned ring_pos = read % NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES;
        unsigned contiguous = NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - ring_pos;
        if (contiguous > available) {
            contiguous = available;
        }
        ma_uint64 input_count = contiguous;
        ma_uint64 output_count = frames - produced;
        ma_result result = ma_data_converter_process_pcm_frames(
            &source->converter, &source->ring[(size_t)ring_pos * NATIVE_AUDIO_PIPELINE_CHANNELS], &input_count,
            &out[produced * NATIVE_AUDIO_PIPELINE_CHANNELS], &output_count);
        if (input_count > 0) {
            atomic_store_explicit(&source->read_cursor, read + (unsigned)input_count, memory_order_release);
        }
        produced += (size_t)output_count;
        if (result != MA_SUCCESS || (input_count == 0 && output_count == 0)) {
            break;
        }
    }
    return produced;
}

static int32_t float_peak_to_i32(float magnitude) {
    if (magnitude <= 0.0f) {
        return 0;
    }
    double scaled = (double)magnitude * 32768.0;
    return scaled >= INT32_MAX ? INT32_MAX : (int32_t)scaled;
}

static void source_apply_gain(NativeAudioSource *source, float *frames, size_t frame_count, float gain_step,
                              float *peak_left, float *peak_right) {
    for (size_t frame = 0; frame < frame_count; frame++) {
        source->current_gain += gain_step;
        float envelope = 1.0f;
        if (source->fade_out_remaining > 0) {
            envelope = (float)source->fade_out_remaining / (float)NATIVE_AUDIO_FADE_FRAMES;
            source->fade_out_remaining--;
        } else if (source->fade_in_remaining > 0) {
            envelope = (float)(NATIVE_AUDIO_FADE_FRAMES - source->fade_in_remaining + 1u) /
                       (float)NATIVE_AUDIO_FADE_FRAMES;
            source->fade_in_remaining--;
        }
        float factor = source->current_gain * envelope;
        float left = frames[frame * 2] * factor;
        float right = frames[frame * 2 + 1] * factor;
        frames[frame * 2] = left;
        frames[frame * 2 + 1] = right;
        float abs_left = left < 0.0f ? -left : left;
        float abs_right = right < 0.0f ? -right : right;
        if (abs_left > *peak_left) {
            *peak_left = abs_left;
        }
        if (abs_right > *peak_right) {
            *peak_right = abs_right;
        }
    }
}

static void source_publish_peaks(NativeAudioSource *source, float left, float right, uint64_t now_ms) {
    atomic_store_explicit(&source->peak_left, (unsigned)float_peak_to_i32(left), memory_order_relaxed);
    atomic_store_explicit(&source->peak_right, (unsigned)float_peak_to_i32(right), memory_order_relaxed);
    atomic_store_explicit(&source->peak_when_ms, (unsigned)now_ms, memory_order_release);
}

static ma_result source_read(ma_data_source *data_source, void *frames_out, ma_uint64 frame_count,
                             ma_uint64 *frames_read) {
    NativeAudioSource *source = (NativeAudioSource *)data_source;
    float *out = (float *)frames_out;
    if (frames_read) {
        *frames_read = 0;
    }
    if (!source || !out || frame_count == 0 || frame_count > SIZE_MAX) {
        return MA_INVALID_ARGS;
    }
    size_t frames = (size_t)frame_count;
    memset(out, 0, frames * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(float));
    (void)source_apply_generation(source);
    (void)source_apply_talkspurt(source);
#if defined(HELLOLG_AUDIO_PIPELINE_TESTING)
    if (source->pipeline->before_ring_read) {
        source->pipeline->before_ring_read(source->pipeline->before_ring_read_ctx, source->index);
    }
#endif
    uint64_t now_ms = pipeline_now_ms(source->pipeline);
    float target_gain = (float)atomic_load_explicit(&source->gain_q15, memory_order_relaxed) / 32768.0f;
    float gain_step = (target_gain - source->current_gain) / (float)frames;
    float peak_left = 0.0f;
    float peak_right = 0.0f;

    if (!atomic_load_explicit(&source->open, memory_order_acquire) || !source->converter_initialized) {
        unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
        atomic_store_explicit(&source->read_cursor, write, memory_order_release);
        source->live = false;
        source->rebuffering = true;
        source_apply_gain(source, out, frames, gain_step, &peak_left, &peak_right);
        source_publish_peaks(source, peak_left, peak_right, now_ms);
        if (frames_read) {
            *frames_read = frame_count;
        }
        return MA_SUCCESS;
    }

    uint32_t target_ms = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    uint32_t queue_ms = source_queue_ms(source);
    bool trim_requested = atomic_load_explicit(&source->trim_requested, memory_order_relaxed);
    bool excessive_backlog = queue_ms > target_ms + NATIVE_AUDIO_HARD_TRIM_ERROR_MS;

    if (source->rebuffering && (trim_requested || excessive_backlog)) {
        if (source_trim_to_target(source) > 0) {
            source->fade_in_remaining = NATIVE_AUDIO_FADE_FRAMES;
        }
        queue_ms = source_queue_ms(source);
        trim_requested = atomic_load_explicit(&source->trim_requested, memory_order_relaxed);
        excessive_backlog = queue_ms > target_ms + NATIVE_AUDIO_HARD_TRIM_ERROR_MS;
    }

    if (source->rebuffering) {
        unsigned last_push = atomic_load_explicit(&source->last_push_ms, memory_order_acquire);
        bool stale_short_burst = queue_ms > 0 && (uint32_t)((unsigned)now_ms - last_push) >= NATIVE_AUDIO_STALE_BURST_MS;
        if (queue_ms >= target_ms || stale_short_burst) {
            source->rebuffering = false;
            source->live = true;
            source->fade_in_remaining = NATIVE_AUDIO_FADE_FRAMES;
        }
    }

    if (!source->live) {
        source->current_correction_ppm = 0;
        atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
        source_apply_gain(source, out, frames, gain_step, &peak_left, &peak_right);
        source_publish_peaks(source, peak_left, peak_right, now_ms);
        if (frames_read) {
            *frames_read = frame_count;
        }
        return MA_SUCCESS;
    }

    if (!source->trim_after_fade && source->fade_out_remaining == 0 && (trim_requested || excessive_backlog)) {
        source->trim_after_fade = true;
        source->fade_out_remaining = NATIVE_AUDIO_FADE_FRAMES;
        source->fade_in_remaining = 0;
    }
    source_update_rate_correction(source);

    size_t offset = 0;
    bool underrun = false;
    while (offset < frames) {
        if (source->trim_after_fade && source->fade_out_remaining == 0) {
            (void)source_trim_to_target(source);
            source->trim_after_fade = false;
            source->fade_in_remaining = NATIVE_AUDIO_FADE_FRAMES;
            source_update_rate_correction(source);
        }
        size_t segment = frames - offset;
        if (source->fade_out_remaining > 0 && segment > source->fade_out_remaining) {
            segment = source->fade_out_remaining;
        }
        bool discontinuity_applied = false;
        size_t produced = source_convert_frames(source, &out[offset * NATIVE_AUDIO_PIPELINE_CHANNELS], segment,
                                                &discontinuity_applied);
        if (!discontinuity_applied && produced < segment) {
            underrun = true;
        }
        size_t gain_frames = discontinuity_applied ? frames - offset : segment;
        source_apply_gain(source, &out[offset * NATIVE_AUDIO_PIPELINE_CHANNELS], gain_frames, gain_step,
                          &peak_left, &peak_right);
        offset += gain_frames;
    }

    if (underrun) {
        atomic_fetch_add_explicit(&source->underruns, 1u, memory_order_relaxed);
        unsigned target = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
        source_raise_target(source, target + NATIVE_AUDIO_TARGET_HEADROOM_MS);
        source->live = false;
        source->rebuffering = true;
        source->trim_after_fade = false;
        source->fade_out_remaining = 0;
        source->fade_in_remaining = 0;
        source->current_correction_ppm = 0;
        atomic_store_explicit(&source->correction_ppm, 0, memory_order_relaxed);
        if (source->converter_initialized) {
            (void)ma_data_converter_reset(&source->converter);
        }
    }

    source_publish_peaks(source, peak_left, peak_right, now_ms);
    if (frames_read) {
        *frames_read = frame_count;
    }
    return MA_SUCCESS;
}

static ma_result source_seek(ma_data_source *data_source, ma_uint64 frame_index) {
    (void)data_source;
    (void)frame_index;
    return MA_NOT_IMPLEMENTED;
}

static ma_result source_get_data_format(ma_data_source *data_source, ma_format *format, ma_uint32 *channels,
                                        ma_uint32 *sample_rate, ma_channel *channel_map, size_t channel_map_cap) {
    (void)data_source;
    if (format) {
        *format = ma_format_f32;
    }
    if (channels) {
        *channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
    }
    if (sample_rate) {
        *sample_rate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
    }
    if (channel_map && channel_map_cap > 0) {
        ma_channel_map_init_standard(ma_standard_channel_map_default, channel_map, channel_map_cap,
                                     NATIVE_AUDIO_PIPELINE_CHANNELS);
    }
    return MA_SUCCESS;
}

static ma_result source_get_cursor(ma_data_source *data_source, ma_uint64 *cursor) {
    (void)data_source;
    if (cursor) {
        *cursor = 0;
    }
    return MA_NOT_IMPLEMENTED;
}

static ma_result source_get_length(ma_data_source *data_source, ma_uint64 *length) {
    (void)data_source;
    if (length) {
        *length = 0;
    }
    return MA_NOT_IMPLEMENTED;
}

static ma_data_source_vtable source_vtable = {
    source_read,
    source_seek,
    source_get_data_format,
    source_get_cursor,
    source_get_length,
    NULL,
    0,
};

static void pipeline_impl_cleanup(NativeAudioPipelineImpl *pipeline) {
    if (!pipeline) {
        return;
    }
    for (int i = NATIVE_AUDIO_PIPELINE_MAX_SOURCES - 1; i >= 0; i--) {
        NativeAudioSource *source = &pipeline->sources[i];
        if (source->node_initialized) {
            ma_data_source_node_uninit(&source->node, NULL);
        }
        if (source->converter_initialized) {
            ma_data_converter_uninit(&source->converter, NULL);
        }
        if (source->base_initialized) {
            ma_data_source_uninit(&source->base);
        }
        free(source->converter_heap);
        free(source->ring);
    }
    if (pipeline->graph_initialized) {
        ma_node_graph_uninit(&pipeline->graph, NULL);
    }
    if (pipeline->control_lock_initialized) {
        pthread_mutex_destroy(&pipeline->control_lock);
    }
    free(pipeline);
}

bool native_audio_pipeline_init_with_clock(NativeAudioPipeline *pipeline, NativeAudioPipelineClock clock,
                                           void *clock_ctx) {
    if (!pipeline || pipeline->impl) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)calloc(1, sizeof(*impl));
    if (!impl) {
        return false;
    }
    impl->clock = clock ? clock : native_audio_monotonic_ms;
    impl->clock_ctx = clock_ctx;
    atomic_init(&impl->output_peak_left, 0u);
    atomic_init(&impl->output_peak_right, 0u);
    atomic_init(&impl->output_peak_when_ms, 0u);
    atomic_init(&impl->pump_stop, false);
    if (pthread_mutex_init(&impl->control_lock, NULL) != 0) {
        pipeline_impl_cleanup(impl);
        return false;
    }
    impl->control_lock_initialized = true;

    ma_node_graph_config graph_config = ma_node_graph_config_init(NATIVE_AUDIO_PIPELINE_CHANNELS);
    graph_config.processingSizeInFrames = NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES;
    if (ma_node_graph_init(&graph_config, NULL, &impl->graph) != MA_SUCCESS) {
        pipeline_impl_cleanup(impl);
        return false;
    }
    impl->graph_initialized = true;

    ma_data_converter_config converter_config = source_converter_config(NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
    size_t converter_heap_size = 0;
    if (ma_data_converter_get_heap_size(&converter_config, &converter_heap_size) != MA_SUCCESS) {
        pipeline_impl_cleanup(impl);
        return false;
    }

    for (int i = 0; i < NATIVE_AUDIO_PIPELINE_MAX_SOURCES; i++) {
        NativeAudioSource *source = &impl->sources[i];
        source->pipeline = impl;
        source->index = i;
        source->ring = (int16_t *)calloc((size_t)NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES *
                                            NATIVE_AUDIO_PIPELINE_CHANNELS,
                                        sizeof(int16_t));
        source->converter_heap = converter_heap_size ? malloc(converter_heap_size) : NULL;
        if (!source->ring || (converter_heap_size && !source->converter_heap)) {
            pipeline_impl_cleanup(impl);
            return false;
        }
        atomic_init(&source->read_cursor, 0u);
        atomic_init(&source->write_cursor, 0u);
        atomic_init(&source->reset_cursor, 0u);
        atomic_init(&source->format_generation, 0u);
        atomic_init(&source->pending_sample_rate, NATIVE_AUDIO_PIPELINE_SAMPLE_RATE);
        atomic_init(&source->pending_channels, NATIVE_AUDIO_PIPELINE_CHANNELS);
        atomic_init(&source->open, false);
        atomic_init(&source->trim_requested, false);
        atomic_init(&source->talkspurt_cursor, 0u);
        atomic_init(&source->talkspurt_generation, 0u);
        atomic_init(&source->last_push_ms, 0u);
        atomic_init(&source->gain_q15, NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
        atomic_init(&source->target_delay_ms, NATIVE_AUDIO_TARGET_INITIAL_MS);
        atomic_init(&source->jitter_p95_ms, 0u);
        atomic_init(&source->correction_ppm, 0);
        atomic_init(&source->underruns, 0u);
        atomic_init(&source->hard_corrections, 0u);
        atomic_init(&source->overflows, 0u);
        atomic_init(&source->peak_left, 0u);
        atomic_init(&source->peak_right, 0u);
        atomic_init(&source->peak_when_ms, 0u);
        source->sample_rate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
        source->channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
        source->producer_sample_rate = NATIVE_AUDIO_PIPELINE_SAMPLE_RATE;
        source->producer_channels = NATIVE_AUDIO_PIPELINE_CHANNELS;
        source->rebuffering = true;
        source->current_gain = 1.0f;

        ma_data_source_config data_source_config = ma_data_source_config_init();
        data_source_config.vtable = &source_vtable;
        if (ma_data_source_init(&data_source_config, &source->base) != MA_SUCCESS) {
            pipeline_impl_cleanup(impl);
            return false;
        }
        source->base_initialized = true;
        ma_data_source_node_config node_config = ma_data_source_node_config_init((ma_data_source *)source);
        if (ma_data_source_node_init(&impl->graph, &node_config, NULL, &source->node) != MA_SUCCESS) {
            pipeline_impl_cleanup(impl);
            return false;
        }
        source->node_initialized = true;
        if (ma_node_attach_output_bus((ma_node *)&source->node, 0, ma_node_graph_get_endpoint(&impl->graph), 0) !=
            MA_SUCCESS) {
            pipeline_impl_cleanup(impl);
            return false;
        }
    }

    pipeline->impl = impl;
    return true;
}

bool native_audio_pipeline_init(NativeAudioPipeline *pipeline) {
    return native_audio_pipeline_init_with_clock(pipeline, NULL, NULL);
}

bool native_audio_pipeline_is_initialized(const NativeAudioPipeline *pipeline) {
    return pipeline && pipeline->impl;
}

void native_audio_pipeline_destroy(NativeAudioPipeline *pipeline) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    native_audio_pipeline_pump_stop(pipeline);
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pipeline->impl = NULL;
    pipeline_impl_cleanup(impl);
}

bool native_audio_pipeline_set_source_format(NativeAudioPipeline *pipeline, int source_index, uint32_t sample_rate,
                                             uint16_t channels) {
    if (!pipeline || !pipeline->impl || !source_index_valid(source_index) || sample_rate < 8000u ||
        sample_rate > 192000u || channels == 0 || channels > NATIVE_AUDIO_PIPELINE_CHANNELS) {
        return false;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    source->producer_sample_rate = sample_rate;
    source->producer_channels = channels;
    source->have_arrival = false;
    source_jitter_clear(source);
    atomic_store_explicit(&source->target_delay_ms, NATIVE_AUDIO_TARGET_INITIAL_MS, memory_order_relaxed);
    source->seen_underruns = atomic_load_explicit(&source->underruns, memory_order_relaxed);
    source->last_target_change_ms = pipeline_now_ms(source->pipeline);
    unsigned boundary = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    atomic_store_explicit(&source->pending_sample_rate, sample_rate, memory_order_relaxed);
    atomic_store_explicit(&source->pending_channels, channels, memory_order_relaxed);
    atomic_store_explicit(&source->reset_cursor, boundary, memory_order_release);
    atomic_fetch_add_explicit(&source->format_generation, 1u, memory_order_release);
    atomic_store_explicit(&source->open, true, memory_order_release);
    return true;
}

void native_audio_pipeline_close_source(NativeAudioPipeline *pipeline, int source_index) {
    if (!pipeline || !pipeline->impl || !source_index_valid(source_index)) {
        return;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    atomic_store_explicit(&source->open, false, memory_order_release);
    unsigned boundary = atomic_load_explicit(&source->write_cursor, memory_order_acquire);
    atomic_store_explicit(&source->reset_cursor, boundary, memory_order_release);
    atomic_fetch_add_explicit(&source->format_generation, 1u, memory_order_release);
    source->have_arrival = false;
}

void native_audio_pipeline_set_source_gain(NativeAudioPipeline *pipeline, int source_index, int32_t gain_q15) {
    if (!pipeline || !pipeline->impl || !source_index_valid(source_index)) {
        return;
    }
    if (gain_q15 < 0) {
        gain_q15 = 0;
    }
    if (gain_q15 > NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15) {
        gain_q15 = NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    atomic_store_explicit(&source->gain_q15, gain_q15, memory_order_relaxed);
}

size_t native_audio_pipeline_push(NativeAudioPipeline *pipeline, int source_index, const int16_t *samples,
                                  size_t frames, uint32_t timestamp_ms) {
    if (!pipeline || !pipeline->impl || !source_index_valid(source_index) || !samples || frames == 0) {
        return 0;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    if (!atomic_load_explicit(&source->open, memory_order_acquire) || source->producer_sample_rate == 0 ||
        source->producer_channels == 0) {
        return 0;
    }
    uint64_t now_ms = pipeline_now_ms(source->pipeline);
    unsigned write = atomic_load_explicit(&source->write_cursor, memory_order_relaxed);
    source_update_arrival_controller(source, frames, timestamp_ms, now_ms, write);
    unsigned read = atomic_load_explicit(&source->read_cursor, memory_order_acquire);
    unsigned used = write - read;
    unsigned available = used < NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES
                             ? NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - used
                             : 0u;
    size_t to_write = frames < available ? frames : available;
    size_t dropped = frames - to_write;
    if (dropped > 0) {
        atomic_store_explicit(&source->trim_requested, true, memory_order_release);
        atomic_fetch_add_explicit(&source->overflows, 1u, memory_order_relaxed);
    }

    size_t copied = 0;
    while (copied < to_write) {
        unsigned ring_pos = (write + (unsigned)copied) % NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES;
        size_t contiguous = NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES - ring_pos;
        if (contiguous > to_write - copied) {
            contiguous = to_write - copied;
        }
        int16_t *dest = &source->ring[(size_t)ring_pos * NATIVE_AUDIO_PIPELINE_CHANNELS];
        if (source->producer_channels == NATIVE_AUDIO_PIPELINE_CHANNELS) {
            memcpy(dest, &samples[copied * NATIVE_AUDIO_PIPELINE_CHANNELS],
                   contiguous * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < contiguous; i++) {
                int16_t mono;
                memcpy(&mono, &samples[copied + i], sizeof(mono));
                dest[i * 2] = mono;
                dest[i * 2 + 1] = mono;
            }
        }
        copied += contiguous;
    }
    if (to_write > 0) {
        atomic_store_explicit(&source->write_cursor, write + (unsigned)to_write, memory_order_release);
        atomic_store_explicit(&source->last_push_ms, (unsigned)now_ms, memory_order_release);
    }
    return dropped;
}

#if defined(HELLOLG_AUDIO_PIPELINE_TESTING)
void native_audio_pipeline_set_test_before_ring_read(NativeAudioPipeline *pipeline,
                                                     NativeAudioPipelineTestHook hook, void *ctx) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    impl->before_ring_read = hook;
    impl->before_ring_read_ctx = ctx;
}
#endif

bool native_audio_pipeline_read_f32(NativeAudioPipeline *pipeline, float *out, size_t frames) {
    if (!pipeline || !pipeline->impl || !out || frames == 0) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    ma_uint64 frames_read = 0;
    ma_result result = ma_node_graph_read_pcm_frames(&impl->graph, out, frames, &frames_read);
    if (frames_read < frames) {
        memset(&out[(size_t)frames_read * NATIVE_AUDIO_PIPELINE_CHANNELS], 0,
               (frames - (size_t)frames_read) * NATIVE_AUDIO_PIPELINE_CHANNELS * sizeof(float));
    }
    float peak_left = 0.0f;
    float peak_right = 0.0f;
    for (size_t frame = 0; frame < frames; frame++) {
        float left = out[frame * 2];
        float right = out[frame * 2 + 1];
        left = left < 0.0f ? -left : left;
        right = right < 0.0f ? -right : right;
        if (left > peak_left) {
            peak_left = left;
        }
        if (right > peak_right) {
            peak_right = right;
        }
    }
    uint64_t now_ms = pipeline_now_ms(impl);
    atomic_store_explicit(&impl->output_peak_left, (unsigned)float_peak_to_i32(peak_left), memory_order_relaxed);
    atomic_store_explicit(&impl->output_peak_right, (unsigned)float_peak_to_i32(peak_right), memory_order_relaxed);
    atomic_store_explicit(&impl->output_peak_when_ms, (unsigned)now_ms, memory_order_release);
    return result == MA_SUCCESS;
}

static int16_t float_to_s16(float sample) {
    if (sample >= 1.0f) {
        return INT16_MAX;
    }
    if (sample <= -1.0f) {
        return INT16_MIN;
    }
    float scaled = sample * 32768.0f;
    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

bool native_audio_pipeline_read_s16(NativeAudioPipeline *pipeline, int16_t *out, size_t frames) {
    if (!pipeline || !pipeline->impl || !out || frames == 0) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    size_t offset = 0;
    bool ok = true;
    while (offset < frames) {
        size_t block = frames - offset;
        if (block > NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES) {
            block = NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES;
        }
        if (!native_audio_pipeline_read_f32(pipeline, impl->conversion_buffer, block)) {
            ok = false;
        }
        for (size_t i = 0; i < block * NATIVE_AUDIO_PIPELINE_CHANNELS; i++) {
            out[offset * NATIVE_AUDIO_PIPELINE_CHANNELS + i] = float_to_s16(impl->conversion_buffer[i]);
        }
        offset += block;
    }
    return ok;
}

static void get_stale_aware_peaks(NativeAudioPipelineImpl *impl, atomic_uint *left_peak, atomic_uint *right_peak,
                                  atomic_uint *when_ms, int32_t *left, int32_t *right) {
    if (left) {
        *left = 0;
    }
    if (right) {
        *right = 0;
    }
    unsigned when = atomic_load_explicit(when_ms, memory_order_acquire);
    unsigned now = (unsigned)pipeline_now_ms(impl);
    if ((uint32_t)(now - when) > 100u) {
        return;
    }
    if (left) {
        *left = (int32_t)atomic_load_explicit(left_peak, memory_order_relaxed);
    }
    if (right) {
        *right = (int32_t)atomic_load_explicit(right_peak, memory_order_relaxed);
    }
}

void native_audio_pipeline_get_source_peaks(NativeAudioPipeline *pipeline, int source_index, int32_t *left,
                                            int32_t *right) {
    if (!pipeline || !pipeline->impl || !source_index_valid(source_index)) {
        if (left) {
            *left = 0;
        }
        if (right) {
            *right = 0;
        }
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    NativeAudioSource *source = &impl->sources[source_index];
    get_stale_aware_peaks(impl, &source->peak_left, &source->peak_right, &source->peak_when_ms, left, right);
}

void native_audio_pipeline_get_output_peaks(NativeAudioPipeline *pipeline, int32_t *left, int32_t *right) {
    if (!pipeline || !pipeline->impl) {
        if (left) {
            *left = 0;
        }
        if (right) {
            *right = 0;
        }
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    get_stale_aware_peaks(impl, &impl->output_peak_left, &impl->output_peak_right, &impl->output_peak_when_ms,
                          left, right);
}

bool native_audio_pipeline_get_source_stats(NativeAudioPipeline *pipeline, int source_index,
                                            NativeAudioSourceStats *stats) {
    if (!stats) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));
    if (!pipeline || !pipeline->impl || !source_index_valid(source_index)) {
        return false;
    }
    NativeAudioSource *source = &((NativeAudioPipelineImpl *)pipeline->impl)->sources[source_index];
    stats->queue_ms = source_queue_ms(source);
    stats->target_delay_ms = atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed);
    stats->jitter_p95_ms = atomic_load_explicit(&source->jitter_p95_ms, memory_order_relaxed);
    stats->src_correction_ppm = atomic_load_explicit(&source->correction_ppm, memory_order_relaxed);
    stats->underruns = atomic_load_explicit(&source->underruns, memory_order_relaxed);
    stats->hard_corrections = atomic_load_explicit(&source->hard_corrections, memory_order_relaxed);
    stats->overflows = atomic_load_explicit(&source->overflows, memory_order_relaxed);
    return true;
}

static void pipeline_log_stats(NativeAudioPipelineImpl *pipeline) {
    uint64_t now_ms = pipeline_now_ms(pipeline);
    if (now_ms - pipeline->last_stats_log_ms < NATIVE_AUDIO_STATS_LOG_MS) {
        return;
    }
    pipeline->last_stats_log_ms = now_ms;
    for (int i = 0; i < NATIVE_AUDIO_PIPELINE_MAX_SOURCES; i++) {
        NativeAudioSource *source = &pipeline->sources[i];
        if (!atomic_load_explicit(&source->open, memory_order_relaxed)) {
            continue;
        }
        fprintf(stderr,
                "[native-audio] source %d queue=%ums target=%ums jitter-p95=%ums src=%dppm underruns=%u hard=%u overflow=%u\n",
                i, source_queue_ms(source),
                atomic_load_explicit(&source->target_delay_ms, memory_order_relaxed),
                atomic_load_explicit(&source->jitter_p95_ms, memory_order_relaxed),
                atomic_load_explicit(&source->correction_ppm, memory_order_relaxed),
                atomic_load_explicit(&source->underruns, memory_order_relaxed),
                atomic_load_explicit(&source->hard_corrections, memory_order_relaxed),
                atomic_load_explicit(&source->overflows, memory_order_relaxed));
    }
}

static void timespec_add_ns(struct timespec *time, uint64_t nanoseconds) {
    time->tv_sec += (time_t)(nanoseconds / 1000000000u);
    time->tv_nsec += (long)(nanoseconds % 1000000000u);
    if (time->tv_nsec >= 1000000000L) {
        time->tv_sec++;
        time->tv_nsec -= 1000000000L;
    }
}

static bool timespec_after(const struct timespec *left, const struct timespec *right) {
    return left->tv_sec > right->tv_sec || (left->tv_sec == right->tv_sec && left->tv_nsec > right->tv_nsec);
}

static void *pipeline_pump_main(void *ctx) {
    NativeAudioPipelineImpl *pipeline = (NativeAudioPipelineImpl *)ctx;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!atomic_load_explicit(&pipeline->pump_stop, memory_order_acquire)) {
        NativeAudioPipeline wrapper = {.impl = pipeline};
        (void)native_audio_pipeline_read_s16(&wrapper, pipeline->pump_buffer,
                                             NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES);
        pipeline->feed(pipeline->feed_ctx, pipeline->pump_buffer, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES);
        pipeline_log_stats(pipeline);

        timespec_add_ns(&next, 10000000u);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_after(&now, &next)) {
            /* A blocked NDL feed is an outage, not cadence debt. Resume one block from
             * now; each source's backlog controller will trim its own old audio. */
            next = now;
            timespec_add_ns(&next, 10000000u);
        }
        while (!atomic_load_explicit(&pipeline->pump_stop, memory_order_acquire) &&
               clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {
        }
    }
    return NULL;
}

bool native_audio_pipeline_pump_start(NativeAudioPipeline *pipeline,
                                      void (*feed)(void *ctx, const int16_t *samples, size_t frames), void *feed_ctx) {
    if (!pipeline || !pipeline->impl || !feed) {
        return false;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pthread_mutex_lock(&impl->control_lock);
    if (impl->pump_running) {
        pthread_mutex_unlock(&impl->control_lock);
        return false;
    }
    impl->feed = feed;
    impl->feed_ctx = feed_ctx;
    impl->last_stats_log_ms = pipeline_now_ms(impl);
    atomic_store_explicit(&impl->pump_stop, false, memory_order_release);
    if (pthread_create(&impl->pump_thread, NULL, pipeline_pump_main, impl) != 0) {
        impl->feed = NULL;
        impl->feed_ctx = NULL;
        pthread_mutex_unlock(&impl->control_lock);
        return false;
    }
    impl->pump_running = true;
    pthread_mutex_unlock(&impl->control_lock);
    return true;
}

void native_audio_pipeline_pump_stop(NativeAudioPipeline *pipeline) {
    if (!pipeline || !pipeline->impl) {
        return;
    }
    NativeAudioPipelineImpl *impl = (NativeAudioPipelineImpl *)pipeline->impl;
    pthread_mutex_lock(&impl->control_lock);
    if (!impl->pump_running) {
        pthread_mutex_unlock(&impl->control_lock);
        return;
    }
    atomic_store_explicit(&impl->pump_stop, true, memory_order_release);
    pthread_join(impl->pump_thread, NULL);
    impl->pump_running = false;
    impl->feed = NULL;
    impl->feed_ctx = NULL;
    pthread_mutex_unlock(&impl->control_lock);
}
