#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_pipeline.h"

typedef struct FakeClock {
    uint64_t now_ms;
} FakeClock;

static uint64_t fake_now(void *ctx) {
    return ((FakeClock *)ctx)->now_ms;
}

static void setup(NativeAudioPipeline *pipeline, FakeClock *clock) {
    memset(pipeline, 0, sizeof(*pipeline));
    memset(clock, 0, sizeof(*clock));
    assert(native_audio_pipeline_init_with_clock(pipeline, fake_now, clock));
}

static void fill_s16(int16_t *samples, size_t frames, uint16_t channels, int16_t value) {
    for (size_t i = 0; i < frames * channels; i++) {
        samples[i] = value;
    }
}

static void push_constant(NativeAudioPipeline *pipeline, int source, size_t frames, uint16_t channels,
                          int16_t value, uint32_t timestamp_ms) {
    int16_t block[1024 * 2];
    fill_s16(block, 1024, channels, value);
    while (frames > 0) {
        size_t count = frames > 1024 ? 1024 : frames;
        assert(native_audio_pipeline_push(pipeline, source, block, count, timestamp_ms) == 0);
        frames -= count;
    }
}

static int average_left(const int16_t *samples, size_t frames, size_t begin) {
    int64_t sum = 0;
    for (size_t frame = begin; frame < frames; frame++) {
        sum += samples[frame * 2];
    }
    return (int)(sum / (int64_t)(frames - begin));
}

static void test_init_and_validation(void) {
    assert(!native_audio_pipeline_init(NULL));
    NativeAudioPipeline pipeline = {0};
    FakeClock clock = {0};
    assert(native_audio_pipeline_init_with_clock(&pipeline, fake_now, &clock));
    assert(native_audio_pipeline_is_initialized(&pipeline));
    assert(!native_audio_pipeline_init(&pipeline));
    assert(!native_audio_pipeline_set_source_format(&pipeline, -1, 48000, 2));
    assert(!native_audio_pipeline_set_source_format(&pipeline, 0, 0, 2));
    assert(!native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 3));
    native_audio_pipeline_destroy(&pipeline);
    native_audio_pipeline_destroy(&pipeline);
    assert(!native_audio_pipeline_is_initialized(&pipeline));
}

static void test_mixed_rates_and_resampling_duration(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 44100, 2));
    assert(native_audio_pipeline_set_source_format(&pipeline, 1, 48000, 2));
    push_constant(&pipeline, 0, 4410, 2, 1000, 100);
    push_constant(&pipeline, 1, 4800, 2, 2000, 100);

    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    clock.now_ms += 10;
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    int mixed = average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 32);
    assert(mixed > 2850 && mixed < 3150);

    /* The two converters drain in the same wall-clock neighborhood despite having
     * different input frame counts; no global format re-pin exists. */
    unsigned blocks = 2;
    NativeAudioSourceStats a = {0};
    NativeAudioSourceStats b = {0};
    while (blocks < 16) {
        clock.now_ms += 10;
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        blocks++;
        assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &a));
        assert(native_audio_pipeline_get_source_stats(&pipeline, 1, &b));
        if (a.underruns && b.underruns) {
            break;
        }
    }
    assert(blocks >= 9 && blocks <= 12);
    assert(a.underruns > 0 && b.underruns > 0);
    native_audio_pipeline_destroy(&pipeline);
}

static void deliver_regular_packet(NativeAudioPipeline *pipeline, FakeClock *clock, uint32_t *timestamp,
                                   uint32_t arrival_step_ms, size_t frames) {
    int16_t samples[960 * 2];
    fill_s16(samples, frames, 2, 500);
    clock->now_ms += arrival_step_ms;
    *timestamp += 20u;
    assert(native_audio_pipeline_push(pipeline, 0, samples, frames, *timestamp) == 0);
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    assert(native_audio_pipeline_read_s16(pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    assert(native_audio_pipeline_read_s16(pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
}

static void test_jitter_target_growth_and_decay(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    uint32_t timestamp = 1000;
    for (int i = 0; i < 600; i++) {
        deliver_regular_packet(&pipeline, &clock, &timestamp, 20, 960);
    }
    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.target_delay_ms == 40);

    deliver_regular_packet(&pipeline, &clock, &timestamp, 60, 960);
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.target_delay_ms >= 60);

    /* A 230 ms TCP/HOL stall against a 20 ms capture step is an immediate peak. */
    deliver_regular_packet(&pipeline, &clock, &timestamp, 230, 960);
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.target_delay_ms == 150);

    /* Stable delivery can only remove 10 ms per five seconds. */
    for (int i = 0; i < 3000; i++) {
        deliver_regular_packet(&pipeline, &clock, &timestamp, 20, 960);
    }
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.target_delay_ms == 40);
    assert(stats.jitter_p95_ms == 0);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_timestamp_wrap_fallback_and_silence_reset(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    int16_t packet[960 * 2];
    fill_s16(packet, 960, 2, 100);

    clock.now_ms = 20;
    assert(native_audio_pipeline_push(&pipeline, 0, packet, 960, UINT32_MAX - 10u) == 0);
    clock.now_ms = 40;
    assert(native_audio_pipeline_push(&pipeline, 0, packet, 960, 9u) == 0); /* wrapped delta=20 */
    clock.now_ms = 60;
    assert(native_audio_pipeline_push(&pipeline, 0, packet, 960, 9u) == 0); /* repeated -> duration */
    clock.now_ms = 80;
    assert(native_audio_pipeline_push(&pipeline, 0, packet, 960, 0u) == 0); /* zero -> duration */
    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.jitter_p95_ms == 0);

    clock.now_ms += 230;
    assert(native_audio_pipeline_push(&pipeline, 0, packet, 960, 20u) == 0);
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.target_delay_ms == 150);
    clock.now_ms += 501;
    assert(native_audio_pipeline_push(&pipeline, 0, packet, 960, 40u) == 0);
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.target_delay_ms == 60);
    assert(stats.jitter_p95_ms == 0);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_talkspurt_drops_pre_gap_frames(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));

    clock.now_ms = 100;
    push_constant(&pipeline, 0, 2400, 2, 1000, 100); /* 50 ms left behind by a stalled sink. */
    clock.now_ms += 501;
    push_constant(&pipeline, 0, 2880, 2, 2000, 120); /* First 60 ms of the new talkspurt. */

    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    assert(average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 300) > 1900);

    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.queue_ms >= 49 && stats.queue_ms <= 51);
    assert(stats.hard_corrections == 0);
    native_audio_pipeline_destroy(&pipeline);
}

typedef struct FormatRaceHook {
    NativeAudioPipeline *pipeline;
    bool fired;
} FormatRaceHook;

static void publish_format_during_render(void *ctx, int source) {
    FormatRaceHook *hook = (FormatRaceHook *)ctx;
    if (source != 0 || hook->fired) {
        return;
    }
    hook->fired = true;
    assert(native_audio_pipeline_set_source_format(hook->pipeline, 0, 44100, 2));
    push_constant(hook->pipeline, 0, 4410, 2, 2000, 200);
}

static void test_format_generation_rechecked_before_ring_read(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    push_constant(&pipeline, 0, 4800, 2, 1000, 100);

    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    NativeAudioSourceStats stats;
    for (int block = 0; block < 12; block++) {
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
        if (stats.queue_ms == 0) {
            break;
        }
    }
    assert(stats.queue_ms == 0);
    assert(stats.underruns == 0);

    FormatRaceHook hook = {.pipeline = &pipeline};
    native_audio_pipeline_set_test_before_ring_read(&pipeline, publish_format_during_render, &hook);
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    native_audio_pipeline_set_test_before_ring_read(&pipeline, NULL, NULL);
    assert(hook.fired);
    assert(average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 0) == 0);
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.queue_ms == 100);
    assert(stats.underruns == 0);

    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    assert(average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 300) > 1900);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_source_format_reset_is_local(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    assert(native_audio_pipeline_set_source_format(&pipeline, 1, 48000, 2));
    push_constant(&pipeline, 0, 4800, 2, 1000, 100);
    push_constant(&pipeline, 1, 4800, 2, 100, 100);
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    clock.now_ms += 10;

    /* Old source-0 frames must not leak past its 48k -> 44.1k generation boundary;
     * source 1 continues uninterrupted. */
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 44100, 2));
    push_constant(&pipeline, 0, 4410, 2, 2000, 200);
    for (int i = 0; i < 2; i++) {
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        clock.now_ms += 10;
    }
    int mixed = average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 32);
    assert(mixed > 2000 && mixed < 2200);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_underrun_isolated_from_other_source(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    assert(native_audio_pipeline_set_source_format(&pipeline, 1, 48000, 2));
    push_constant(&pipeline, 0, 4800, 2, 1000, 100);
    push_constant(&pipeline, 1, 2880, 2, 2000, 100);

    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    for (int block = 0; block < 12; block++) {
        if (block > 0) {
            push_constant(&pipeline, 0, 480, 2, 1000, 100u + (uint32_t)block * 10u);
        }
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        clock.now_ms += 10;
    }
    NativeAudioSourceStats a;
    NativeAudioSourceStats b;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &a));
    assert(native_audio_pipeline_get_source_stats(&pipeline, 1, &b));
    assert(a.underruns == 0);
    assert(b.underruns > 0);
    int remaining = average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 32);
    assert(remaining > 900 && remaining < 1100);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_hard_trim_fades_and_recovers_sink_stall(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    push_constant(&pipeline, 0, 4800, 2, 10000, 100);
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    for (int i = 0; i < 2; i++) {
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        clock.now_ms += 10;
    }

    /* Simulate audio arriving while the sink is blocked for 200 ms. */
    push_constant(&pipeline, 0, 9600, 2, 10000, 300);
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    assert(out[0] > 9000);
    assert(out[238 * 2] < 1000);
    assert(out[241 * 2] < 1000);
    assert(out[479 * 2] > 9000);
    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.hard_corrections == 1);
    assert(stats.queue_ms <= stats.target_delay_ms + 20);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_overflow_requests_consumer_trim(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    int16_t *full = (int16_t *)malloc((size_t)NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES * 2 * sizeof(int16_t));
    assert(full);
    fill_s16(full, NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES, 2, 77);
    assert(native_audio_pipeline_push(&pipeline, 0, full, NATIVE_AUDIO_PIPELINE_CAPACITY_FRAMES, 100) == 0);
    int16_t extra[480 * 2];
    fill_s16(extra, 480, 2, 88);
    assert(native_audio_pipeline_push(&pipeline, 0, extra, 480, 110) == 480);
    free(full);

    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.overflows == 1);
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.hard_corrections == 1);
    assert(stats.queue_ms <= stats.target_delay_ms + 20);
    native_audio_pipeline_destroy(&pipeline);
}

static void run_drift_case(int frames_per_tick, bool expect_positive) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    push_constant(&pipeline, 0, 3360, 2, 1000, 0); /* target + 10 ms margin */
    int16_t in[481 * 2];
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    fill_s16(in, (size_t)frames_per_tick, 2, 1000);
    uint32_t timestamp = 0;
    for (int tick = 0; tick < 10000; tick++) {
        clock.now_ms += 10;
        timestamp += 10;
        assert(native_audio_pipeline_push(&pipeline, 0, in, (size_t)frames_per_tick, timestamp) == 0);
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    }
    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    if (expect_positive) {
        assert(stats.src_correction_ppm > 0);
    } else {
        assert(stats.src_correction_ppm < 0);
    }
    assert(stats.queue_ms >= 30 && stats.queue_ms <= 110);
    assert(stats.underruns == 0);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_clock_drift_both_directions(void) {
    run_drift_case(481, true);
    run_drift_case(479, false);
}

static void test_gain_meters_clipping_and_reopen(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    assert(native_audio_pipeline_set_source_format(&pipeline, 1, 48000, 2));
    native_audio_pipeline_set_source_gain(&pipeline, 0, NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15 / 2);
    push_constant(&pipeline, 0, 4800, 2, 20000, 100);
    push_constant(&pipeline, 1, 4800, 2, 20000, 100);
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    for (int i = 0; i < 2; i++) {
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        clock.now_ms += 10;
    }
    int mixed = average_left(out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 32);
    assert(mixed > 29000 && mixed < 31000);
    int32_t left;
    int32_t right;
    native_audio_pipeline_get_source_peaks(&pipeline, 0, &left, &right);
    assert(left > 9500 && left < 10500 && right == left);
    native_audio_pipeline_get_output_peaks(&pipeline, &left, &right);
    assert(left > 29000 && left < 31000);

    native_audio_pipeline_set_source_gain(&pipeline, 0, NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15);
    native_audio_pipeline_set_source_gain(&pipeline, 1, NATIVE_AUDIO_PIPELINE_GAIN_MAX_Q15);
    assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    native_audio_pipeline_get_output_peaks(&pipeline, &left, &right);
    assert(left > 32768);
    assert(out[479 * 2] == INT16_MAX);

    native_audio_pipeline_close_source(&pipeline, 1);
    native_audio_pipeline_close_source(&pipeline, 0);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 1));
    push_constant(&pipeline, 0, 4800, 1, 1000, 200);
    for (int i = 0; i < 2; i++) {
        assert(native_audio_pipeline_read_s16(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        clock.now_ms += 10;
    }
    native_audio_pipeline_get_source_peaks(&pipeline, 0, &left, &right);
    assert(left > 1900 && left < 2100 && right == left); /* gain survived; mono duplicated */
    clock.now_ms += 101;
    native_audio_pipeline_get_source_peaks(&pipeline, 0, &left, &right);
    assert(left == 0 && right == 0);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_render_contract_smoke(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    push_constant(&pipeline, 0, 4800, 2, 1234, 100);
    float out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    /* This loop drives format application, rebuffer, drift updates and underrun from
     * the callback-safe entry point. The implementation contains no allocator, logger,
     * mutex or wait on this path; miniaudio's graph cache was allocated by init. */
    for (int i = 0; i < 32; i++) {
        assert(native_audio_pipeline_read_f32(&pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
        clock.now_ms += 10;
    }
    native_audio_pipeline_destroy(&pipeline);
}

static atomic_uint fed_blocks;

static void count_feed(void *ctx, const int16_t *samples, size_t frames) {
    (void)ctx;
    assert(samples);
    assert(frames == NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES);
    atomic_fetch_add(&fed_blocks, 1u);
}

static void test_pump_delivers_and_stops(void) {
    NativeAudioPipeline pipeline = {0};
    assert(native_audio_pipeline_init(&pipeline));
    atomic_store(&fed_blocks, 0u);
    assert(native_audio_pipeline_pump_start(&pipeline, count_feed, NULL));
    for (int i = 0; i < 1000 && atomic_load(&fed_blocks) < 3u; i++) {
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    assert(atomic_load(&fed_blocks) >= 3u);
    native_audio_pipeline_pump_stop(&pipeline);
    unsigned stopped = atomic_load(&fed_blocks);
    struct timespec pause = {0, 30000000L};
    nanosleep(&pause, NULL);
    assert(atomic_load(&fed_blocks) == stopped);
    native_audio_pipeline_destroy(&pipeline);
}

int main(void) {
    test_init_and_validation();
    test_mixed_rates_and_resampling_duration();
    test_jitter_target_growth_and_decay();
    test_timestamp_wrap_fallback_and_silence_reset();
    test_talkspurt_drops_pre_gap_frames();
    test_format_generation_rechecked_before_ring_read();
    test_source_format_reset_is_local();
    test_underrun_isolated_from_other_source();
    test_hard_trim_fades_and_recovers_sink_stall();
    test_overflow_requests_consumer_trim();
    test_clock_drift_both_directions();
    test_gain_meters_clipping_and_reopen();
    test_render_contract_smoke();
    test_pump_delivers_and_stops();
    printf("test_audio_pipeline: all tests passed\n");
    return 0;
}
