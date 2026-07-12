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
    NativeAudioSourceStats stats;
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(!stats.open);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(stats.open);
    native_audio_pipeline_close_source(&pipeline, 0);
    assert(native_audio_pipeline_get_source_stats(&pipeline, 0, &stats));
    assert(!stats.open);
    native_audio_pipeline_destroy(&pipeline);
    native_audio_pipeline_destroy(&pipeline);
    assert(!native_audio_pipeline_is_initialized(&pipeline));
    native_audio_pipeline_set_duck_foreground(NULL, 0, 0xFu);
    native_audio_pipeline_set_duck_foreground(&pipeline, 0, 0xFu);
    native_audio_pipeline_set_source_muted(NULL, 0, true);
    native_audio_pipeline_set_source_muted(&pipeline, -1, true);
    native_audio_pipeline_set_source_muted(&pipeline, 0, true);
    native_audio_pipeline_set_solo_mask(NULL, 0xFFu);
    native_audio_pipeline_set_solo_mask(&pipeline, 0xFFu);
    assert(native_audio_pipeline_get_duck_factor_q15(NULL) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
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

/* Pushes one 10 ms block into sources 0/1 (negative value = skip) and renders one block.
 * Timestamps track the fake clock so the arrival controller sees a jitter-free feed. */
static void duck_step(NativeAudioPipeline *pipeline, FakeClock *clock, int value0, int value1) {
    if (value0 >= 0) {
        push_constant(pipeline, 0, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 2, (int16_t)value0, (uint32_t)clock->now_ms);
    }
    if (value1 >= 0) {
        push_constant(pipeline, 1, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES, 2, (int16_t)value1, (uint32_t)clock->now_ms);
    }
    int16_t out[NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES * 2];
    assert(native_audio_pipeline_read_s16(pipeline, out, NATIVE_AUDIO_PIPELINE_BLOCK_FRAMES));
    clock->now_ms += 10;
}

static int32_t source_peak(NativeAudioPipeline *pipeline, int source) {
    int32_t left = 0;
    int32_t right = 0;
    native_audio_pipeline_get_source_peaks(pipeline, source, &left, &right);
    assert(left == right); /* every duck test feeds identical stereo values */
    return left;
}

static void duck_setup_two_sources(NativeAudioPipeline *pipeline, FakeClock *clock, int foreground) {
    setup(pipeline, clock);
    assert(native_audio_pipeline_set_source_format(pipeline, 0, 48000, 2));
    assert(native_audio_pipeline_set_source_format(pipeline, 1, 48000, 2));
    native_audio_pipeline_set_duck_foreground(pipeline, foreground, 0xFu);
    push_constant(pipeline, 0, 4800, 2, 20000, 0);
    push_constant(pipeline, 1, 4800, 2, 20000, 0);
}

static void test_duck_attack_hold_release(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    /* Block 1 publishes the first peaks, block 2 ramps the attack, block 3 is ducked. */
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t fg = source_peak(&pipeline, 0);
    assert(fg > 4800 && fg < 5300); /* 20000 * 10^(-12/20) */
    int32_t bg = source_peak(&pipeline, 1);
    assert(bg > 19000 && bg < 20500);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_DUCK_GAIN_Q15);

    /* Silence the background: the duck holds, then releases on a timed ramp. */
    uint64_t t_close = clock.now_ms;
    native_audio_pipeline_close_source(&pipeline, 1);
    while (clock.now_ms < t_close + 500) {
        duck_step(&pipeline, &clock, 20000, -1);
    }
    fg = source_peak(&pipeline, 0);
    assert(fg > 4800 && fg < 5300); /* inside the hold window */
    while (clock.now_ms < t_close + 800) {
        duck_step(&pipeline, &clock, 20000, -1);
    }
    fg = source_peak(&pipeline, 0);
    assert(fg > 11000 && fg < 14500); /* mid-release */
    while (clock.now_ms < t_close + 1100) {
        duck_step(&pipeline, &clock, 20000, -1);
    }
    fg = source_peak(&pipeline, 0);
    assert(fg > 19000 && fg < 20500); /* fully released */
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_duck_composes_with_user_fader_and_disable(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    native_audio_pipeline_set_source_gain(&pipeline, 0, NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15 / 2);
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t fg = source_peak(&pipeline, 0);
    assert(fg > 2300 && fg < 2700); /* 20000 * 0.5 fader * -12 dB duck: multiplies, not overwrites */

    /* Disabling (empty trigger mask) releases to the fader level even while the
     * background stays loud. */
    native_audio_pipeline_set_duck_foreground(&pipeline, 0, 0u);
    uint64_t t_disable = clock.now_ms;
    while (clock.now_ms < t_disable + 500) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    fg = source_peak(&pipeline, 0);
    assert(fg > 9500 && fg < 10500); /* user fader survived the duck */
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_duck_foreground_switch(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) < 5300);
    assert(source_peak(&pipeline, 1) > 19000);

    /* Retarget: the duck moves to source 1 through the gain ramps, with no dropout. */
    native_audio_pipeline_set_duck_foreground(&pipeline, 1, 0xFu);
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
        assert(source_peak(&pipeline, 0) > 4000);
        assert(source_peak(&pipeline, 1) > 4000);
    }
    int32_t recovered = source_peak(&pipeline, 0);
    assert(recovered > 19000 && recovered < 20500);
    int32_t ducked = source_peak(&pipeline, 1);
    assert(ducked > 4800 && ducked < 5300);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_duck_ignores_quiet_background(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    setup(&pipeline, &clock);
    assert(native_audio_pipeline_set_source_format(&pipeline, 0, 48000, 2));
    assert(native_audio_pipeline_set_source_format(&pipeline, 1, 48000, 2));
    native_audio_pipeline_set_duck_foreground(&pipeline, 0, 0xFu);
    push_constant(&pipeline, 0, 4800, 2, 20000, 0);
    push_constant(&pipeline, 1, 4800, 2, 200, 0);
    for (int i = 0; i < 5; i++) {
        duck_step(&pipeline, &clock, 20000, 200);
    }
    /* Sub-threshold background noise and the foreground's own loud audio: no duck. */
    int32_t fg = source_peak(&pipeline, 0);
    assert(fg > 19000 && fg < 20500);
    int32_t bg = source_peak(&pipeline, 1);
    assert(bg > 150 && bg < 250);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_duck_respects_trigger_mask(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    /* Source 1 is loud but NOT in the trigger mask: no duck. */
    native_audio_pipeline_set_duck_foreground(&pipeline, 0, 1u << 2);
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t fg = source_peak(&pipeline, 0);
    assert(fg > 19000 && fg < 20500);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);

    /* Adding source 1 to the mask engages the duck within the attack window. */
    native_audio_pipeline_set_duck_foreground(&pipeline, 0, 1u << 1);
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    fg = source_peak(&pipeline, 0);
    assert(fg > 4800 && fg < 5300);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_duck_hold_does_not_transfer_on_switch(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) < 5300); /* duck engaged by loud source 1 */

    /* Mid-hold switch to source 1 with a mask whose only trigger (source 2) is silent:
     * the old hold must NOT keep the new foreground attenuated — the release starts
     * immediately, so well before the old hold would have expired the level is back. */
    native_audio_pipeline_set_duck_foreground(&pipeline, 1, 1u << 2);
    uint64_t t_switch = clock.now_ms;
    while (clock.now_ms < t_switch + 250) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 1) > 10000); /* mid-release already; a leaked hold would pin ~5000 */
    while (clock.now_ms < t_switch + 500) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t recovered = source_peak(&pipeline, 1);
    assert(recovered > 19000 && recovered < 20500);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_mute_silences_and_restores(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    native_audio_pipeline_set_duck_foreground(&pipeline, -1, 0u); /* isolate mute from the duck */
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) > 19000 && source_peak(&pipeline, 1) > 19000);

    native_audio_pipeline_set_source_muted(&pipeline, 1, true);
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 1) < 50); /* ramped to silence, no click */
    int32_t other = source_peak(&pipeline, 0);
    assert(other > 19000 && other < 20500); /* neighbor untouched */

    native_audio_pipeline_set_source_muted(&pipeline, 1, false);
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t restored = source_peak(&pipeline, 1);
    assert(restored > 19000 && restored < 20500);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_solo_isolates_and_mute_wins(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    native_audio_pipeline_set_duck_foreground(&pipeline, -1, 0u);
    native_audio_pipeline_set_solo_mask(&pipeline, 1u << 0);
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) > 19000); /* soloed */
    assert(source_peak(&pipeline, 1) < 50);    /* solo-cut */

    native_audio_pipeline_set_source_muted(&pipeline, 0, true); /* muted AND soloed */
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) < 50); /* mute wins */
    assert(source_peak(&pipeline, 1) < 50);

    native_audio_pipeline_set_source_muted(&pipeline, 0, false);
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) > 19000);

    native_audio_pipeline_set_solo_mask(&pipeline, 0u); /* solo release */
    for (int i = 0; i < 3; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t released = source_peak(&pipeline, 1);
    assert(released > 19000 && released < 20500);
    native_audio_pipeline_destroy(&pipeline);
}

static void test_muted_background_cannot_trigger_duck(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0); /* duck armed: fg 0, mask 0xF */
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    int32_t ducked = source_peak(&pipeline, 0);
    assert(ducked > 4800 && ducked < 5300); /* loud background: duck engaged */

    /* Muting the noisy background releases the duck (hold + release) and, muted, it can
     * no longer re-trigger it no matter how loud its producer pushes. */
    native_audio_pipeline_set_source_muted(&pipeline, 1, true);
    uint64_t t_mute = clock.now_ms;
    while (clock.now_ms < t_mute + 1150) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) > 19000);
    assert(source_peak(&pipeline, 1) < 50);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);

    native_audio_pipeline_set_source_muted(&pipeline, 1, false);
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    ducked = source_peak(&pipeline, 0);
    assert(ducked > 4800 && ducked < 5300); /* unmuted: normal duck attack */
    native_audio_pipeline_destroy(&pipeline);
}

static void test_solo_cut_background_cannot_trigger_duck(void) {
    NativeAudioPipeline pipeline;
    FakeClock clock;
    duck_setup_two_sources(&pipeline, &clock, 0);
    for (int i = 0; i < 4; i++) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) < 5300); /* duck engaged by the loud background */

    /* Soloing the foreground cuts the background, which releases the duck for good. */
    native_audio_pipeline_set_solo_mask(&pipeline, 1u << 0);
    uint64_t t_solo = clock.now_ms;
    while (clock.now_ms < t_solo + 1150) {
        duck_step(&pipeline, &clock, 20000, 20000);
    }
    assert(source_peak(&pipeline, 0) > 19000);
    assert(source_peak(&pipeline, 1) < 50);
    assert(native_audio_pipeline_get_duck_factor_q15(&pipeline) == NATIVE_AUDIO_PIPELINE_GAIN_UNITY_Q15);
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
     * mutex or wait on this path; miniaudio's engine and voice caches were allocated by init. */
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
    test_duck_attack_hold_release();
    test_duck_composes_with_user_fader_and_disable();
    test_duck_foreground_switch();
    test_duck_ignores_quiet_background();
    test_duck_respects_trigger_mask();
    test_duck_hold_does_not_transfer_on_switch();
    test_mute_silences_and_restores();
    test_solo_isolates_and_mute_wins();
    test_muted_background_cannot_trigger_duck();
    test_solo_cut_background_cannot_trigger_duck();
    test_render_contract_smoke();
    test_pump_delivers_and_stops();
    printf("test_audio_pipeline: all tests passed\n");
    return 0;
}
