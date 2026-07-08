#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio_mixer.h"

/* Stereo, tiny chunks so cases stay readable: chunk 4 frames, capacity 16, prebuffer 4. */
#define TEST_RATE 48000u
#define TEST_CHANNELS 2u
#define TEST_CHUNK 4u
#define TEST_CAP 16u
#define TEST_PREBUF 4u

/* Initializes in place: a NativeAudioMixer must never be returned/copied by value —
 * POSIX leaves using a copy of an initialized mutex/condvar undefined. */
static void mixer_setup(NativeAudioMixer *mixer, size_t prebuffer_frames) {
    assert(native_mixer_init(mixer, TEST_RATE, TEST_CHANNELS, TEST_CHUNK, TEST_CAP, prebuffer_frames));
}

static void fill_frames(int16_t *buf, size_t frames, int16_t value) {
    for (size_t i = 0; i < frames * TEST_CHANNELS; i++) {
        buf[i] = value;
    }
}

static void test_init_validation(void) {
    NativeAudioMixer mixer;
    assert(!native_mixer_init(&mixer, 0, 2, 4, 16, 4));
    assert(!native_mixer_init(&mixer, 48000, 0, 4, 16, 4));
    assert(!native_mixer_init(&mixer, 48000, 2, 0, 16, 4));
    assert(!native_mixer_init(&mixer, 48000, 2, 8, 4, 0));   /* capacity < chunk */
    assert(!native_mixer_init(&mixer, 48000, 2, 4, 16, 32)); /* prebuffer > capacity */
    /* A zeroed mixer is safe to destroy / query. */
    NativeAudioMixer dead;
    memset(&dead, 0, sizeof(dead));
    native_mixer_destroy(&dead);
    assert(!native_mixer_ready(&dead));
}

static void test_closed_source_drops_pushes(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    int16_t data[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(data, TEST_CHUNK, 100);
    assert(native_mixer_push(&mixer, 0, data, TEST_CHUNK) == 0);
    assert(!native_mixer_ready(&mixer)); /* source not open: nothing queued */
    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(!native_mixer_pull(&mixer, out));
    native_mixer_destroy(&mixer);
}

static void test_single_source_identity(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);

    int16_t data[TEST_CHUNK * TEST_CHANNELS];
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        data[i] = (int16_t)(i + 1);
    }
    assert(native_mixer_push(&mixer, 0, data, TEST_CHUNK) == 0);
    assert(native_mixer_ready(&mixer));

    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    assert(memcmp(out, data, sizeof(out)) == 0);
    /* Ring drained: not ready anymore. */
    assert(!native_mixer_ready(&mixer));
    native_mixer_destroy(&mixer);
}

static void test_two_sources_are_summed(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    native_mixer_set_source_open(&mixer, 1, true);

    int16_t a[TEST_CHUNK * TEST_CHANNELS];
    int16_t b[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(a, TEST_CHUNK, 1000);
    fill_frames(b, TEST_CHUNK, -250);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_push(&mixer, 1, b, TEST_CHUNK) == 0);

    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 750);
    }
    native_mixer_destroy(&mixer);
}

static void test_sum_saturates(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    native_mixer_set_source_open(&mixer, 1, true);

    int16_t a[TEST_CHUNK * TEST_CHANNELS];
    int16_t b[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(a, TEST_CHUNK, 30000);
    fill_frames(b, TEST_CHUNK, 30000);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_push(&mixer, 1, b, TEST_CHUNK) == 0);
    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 32767);
    }

    fill_frames(a, TEST_CHUNK, -30000);
    fill_frames(b, TEST_CHUNK, -30000);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_push(&mixer, 1, b, TEST_CHUNK) == 0);
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == -32768);
    }
    native_mixer_destroy(&mixer);
}

static void test_three_sources_clamp_once(void) {
    /* Clipping must happen once over the FULL sum: 30000 + 30000 - 30000 = 30000, not
     * saturate(saturate(30000+30000) - 30000) = 2767. */
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    native_mixer_set_source_open(&mixer, 1, true);
    native_mixer_set_source_open(&mixer, 2, true);

    int16_t buf[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(buf, TEST_CHUNK, 30000);
    assert(native_mixer_push(&mixer, 0, buf, TEST_CHUNK) == 0);
    assert(native_mixer_push(&mixer, 1, buf, TEST_CHUNK) == 0);
    fill_frames(buf, TEST_CHUNK, -30000);
    assert(native_mixer_push(&mixer, 2, buf, TEST_CHUNK) == 0);

    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 30000);
    }
    native_mixer_destroy(&mixer);
}

static void test_stale_burst_promoted_while_other_source_plays(void) {
    /* A sub-prebuffer burst on a quiet source must be played out after the stale
     * threshold (>=100ms) even though ANOTHER source keeps contributing the whole time. */
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    native_mixer_set_source_open(&mixer, 1, true);

    int16_t burst[2 * TEST_CHANNELS];
    fill_frames(burst, 2, 7); /* 2 frames < prebuffer(4): not ready by fill level */
    assert(native_mixer_push(&mixer, 1, burst, 2) == 0);

    int16_t a[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(a, TEST_CHUNK, 100);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);

    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    assert(out[0] == 100); /* burst still priming: only source 0 */

    struct timespec ts = {0, 120 * 1000000L}; /* > 100ms stale floor */
    nanosleep(&ts, NULL);

    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_pull(&mixer, out));
    assert(out[0] == 107);                 /* burst promoted by age, mixed in */
    assert(out[2 * TEST_CHANNELS] == 100); /* burst was only 2 frames; rest is source 0 */
    native_mixer_destroy(&mixer);
}

static void test_prebuffer_gates_contribution(void) {
    /* prebuffer 8 frames = 2 chunks: one chunk queued must NOT be ready yet. */
    NativeAudioMixer mixer;
    mixer_setup(&mixer, 8);
    native_mixer_set_source_open(&mixer, 0, true);

    int16_t data[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(data, TEST_CHUNK, 42);
    assert(native_mixer_push(&mixer, 0, data, TEST_CHUNK) == 0);
    assert(!native_mixer_ready(&mixer));
    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(!native_mixer_pull(&mixer, out));

    /* Second chunk completes the prebuffer: both chunks become pullable. */
    assert(native_mixer_push(&mixer, 0, data, TEST_CHUNK) == 0);
    assert(native_mixer_ready(&mixer));
    assert(native_mixer_pull(&mixer, out));
    assert(out[0] == 42);
    /* Still live with data queued: second chunk flows without re-priming. */
    assert(native_mixer_ready(&mixer));
    assert(native_mixer_pull(&mixer, out));
    assert(out[0] == 42);
    native_mixer_destroy(&mixer);
}

static void test_underrun_pads_silence_and_reprimes(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    native_mixer_set_source_open(&mixer, 1, true);

    /* Source 0: plenty. Source 1: primed, then runs dry mid-chunk. */
    int16_t a[2 * TEST_CHUNK * TEST_CHANNELS];
    fill_frames(a, 2 * TEST_CHUNK, 100);
    assert(native_mixer_push(&mixer, 0, a, 2 * TEST_CHUNK) == 0);
    int16_t b[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(b, TEST_CHUNK, 7);
    assert(native_mixer_push(&mixer, 1, b, TEST_CHUNK) == 0);

    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 107);
    }

    /* Source 1 dry -> re-priming: a below-prebuffer trickle must not contribute. */
    int16_t half[2 * TEST_CHANNELS];
    fill_frames(half, 2, 7);
    assert(native_mixer_push(&mixer, 1, half, 2) == 0);
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 100); /* only source 0 */
    }

    /* Trickle reaches the prebuffer (2+2=4 frames): source 1 rejoins the mix. */
    assert(native_mixer_push(&mixer, 1, half, 2) == 0);
    fill_frames(a, TEST_CHUNK, 100);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 107);
    }
    native_mixer_destroy(&mixer);
}

static void test_partial_final_chunk_pads_with_silence(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, 2);
    native_mixer_set_source_open(&mixer, 0, true);

    int16_t data[2 * TEST_CHANNELS];
    fill_frames(data, 2, 500);
    assert(native_mixer_push(&mixer, 0, data, 2) == 0);
    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    for (size_t ch = 0; ch < TEST_CHANNELS * 2; ch++) {
        assert(out[ch] == 500);
    }
    for (size_t i = 2 * TEST_CHANNELS; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 0);
    }
    native_mixer_destroy(&mixer);
}

static void test_capacity_overflow_drops_oldest(void) {
    /* The capacity is the latency cap: overflowing it drops exactly the oldest excess. */
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    int16_t ones[TEST_CAP * TEST_CHANNELS];
    fill_frames(ones, TEST_CAP, 1);
    assert(native_mixer_push(&mixer, 0, ones, TEST_CAP) == 0); /* fits exactly */
    int16_t twos[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(twos, TEST_CHUNK, 2);
    assert(native_mixer_push(&mixer, 0, twos, TEST_CHUNK) == TEST_CHUNK);

    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    size_t chunks = 0;
    int16_t last = 0;
    while (native_mixer_pull(&mixer, out)) {
        chunks++;
        last = out[0];
    }
    assert(chunks == TEST_CAP / TEST_CHUNK); /* capacity's worth, not capacity+1 */
    assert(last == 2);                       /* newest data survived */
    native_mixer_destroy(&mixer);
}

static void test_close_clears_ring(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    int16_t data[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(data, TEST_CHUNK, 9);
    assert(native_mixer_push(&mixer, 0, data, TEST_CHUNK) == 0);
    assert(native_mixer_ready(&mixer));
    native_mixer_set_source_open(&mixer, 0, false);
    assert(!native_mixer_ready(&mixer));
    native_mixer_destroy(&mixer);
}

static void test_source_gain_scales_and_survives_reopen(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, TEST_PREBUF);
    native_mixer_set_source_open(&mixer, 0, true);
    native_mixer_set_source_open(&mixer, 1, true);

    /* Half gain halves (negative samples too); the other source stays at unity. */
    native_mixer_set_source_gain(&mixer, 0, NATIVE_MIXER_GAIN_UNITY_Q15 / 2);
    int16_t a[TEST_CHUNK * TEST_CHANNELS];
    int16_t b[TEST_CHUNK * TEST_CHANNELS];
    fill_frames(a, TEST_CHUNK, -1000);
    fill_frames(b, TEST_CHUNK, 100);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_push(&mixer, 1, b, TEST_CHUNK) == 0);
    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == -400); /* -1000/2 + 100 */
    }
    /* The pull recorded post-gain peaks per source. */
    int32_t left = -1;
    int32_t right = -1;
    native_mixer_get_source_peaks(&mixer, 0, &left, &right);
    assert(left == 500 && right == 500);
    native_mixer_get_source_peaks(&mixer, 1, &left, &right);
    assert(left == 100 && right == 100);

    /* Gain 0 mutes, but the source still consumes at cadence (un-mute stays in sync)
     * and its meter reads silence. */
    native_mixer_set_source_gain(&mixer, 0, 0);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_push(&mixer, 1, b, TEST_CHUNK) == 0);
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 100); /* the muted ring drained alongside */
    }
    assert(!native_mixer_ready(&mixer));
    native_mixer_get_source_peaks(&mixer, 0, &left, &right);
    assert(left == 0 && right == 0);

    /* The gain survives close/reopen: a reconnecting session keeps its setting. */
    native_mixer_set_source_open(&mixer, 0, false);
    native_mixer_set_source_open(&mixer, 0, true);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == 0); /* still muted from before the reopen */
    }

    /* Boost: the cap is 2x unity (+6 dB); out-of-range requests clamp to it. */
    native_mixer_set_source_gain(&mixer, 0, 3 * NATIVE_MIXER_GAIN_MAX_Q15);
    assert(native_mixer_push(&mixer, 0, a, TEST_CHUNK) == 0);
    assert(native_mixer_pull(&mixer, out));
    for (size_t i = 0; i < TEST_CHUNK * TEST_CHANNELS; i++) {
        assert(out[i] == -2000);
    }
    native_mixer_get_source_peaks(&mixer, 0, &left, &right);
    assert(left == 2000 && right == 2000);

    /* Peaks go stale once the pump stops pulling (~100ms): the meter must fall to
     * silence instead of freezing at the last chunk's level. */
    struct timespec ts = {0, 120 * 1000000L};
    nanosleep(&ts, NULL);
    native_mixer_get_source_peaks(&mixer, 0, &left, &right);
    assert(left == 0 && right == 0);
    native_mixer_destroy(&mixer);
}

static void test_zero_prebuffer_is_immediate(void) {
    NativeAudioMixer mixer;
    mixer_setup(&mixer, 0);
    native_mixer_set_source_open(&mixer, 0, true);
    int16_t one_frame[TEST_CHANNELS];
    fill_frames(one_frame, 1, 3);
    assert(native_mixer_push(&mixer, 0, one_frame, 1) == 0);
    assert(native_mixer_ready(&mixer));
    int16_t out[TEST_CHUNK * TEST_CHANNELS];
    assert(native_mixer_pull(&mixer, out));
    assert(out[0] == 3 && out[TEST_CHANNELS] == 0);
    native_mixer_destroy(&mixer);
}

/* Pump smoke test: chunks flow to the feed callback and stop cleanly. */
static atomic_size_t g_fed_frames;

static void counting_feed(void *ctx, const int16_t *samples, size_t frames) {
    (void)ctx;
    (void)samples;
    atomic_fetch_add(&g_fed_frames, frames);
}

static void test_pump_delivers_and_stops(void) {
    NativeAudioMixer mixer;
    /* Real-ish sizes so the pump ticks a few times quickly: 48 frames = 1ms @48k. */
    assert(native_mixer_init(&mixer, 48000, 2, 48, 480, 48));
    native_mixer_set_source_open(&mixer, 0, true);
    atomic_store(&g_fed_frames, 0);
    assert(native_mixer_pump_start(&mixer, counting_feed, NULL));

    int16_t data[480 * 2];
    memset(data, 0x11, sizeof(data));
    assert(native_mixer_push(&mixer, 0, data, 480) == 0);

    /* Wait (bounded) for the pump to drain everything it can. */
    for (int i = 0; i < 2000 && atomic_load(&g_fed_frames) < 480; i++) {
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    assert(atomic_load(&g_fed_frames) >= 480);
    native_mixer_pump_stop(&mixer);
    size_t after_stop = atomic_load(&g_fed_frames);
    struct timespec ts = {0, 20000000}; /* 20ms */
    nanosleep(&ts, NULL);
    assert(atomic_load(&g_fed_frames) == after_stop);
    native_mixer_destroy(&mixer);
}

/* A burst smaller than the prebuffer must drain via the stale-flush timeout instead of
 * waiting for a future burst. */
static void test_pump_flushes_stale_sub_prebuffer_burst(void) {
    NativeAudioMixer mixer;
    /* prebuffer 480 frames = 10ms @48k; stale flush floor is 100ms. */
    assert(native_mixer_init(&mixer, 48000, 2, 48, 4800, 480));
    native_mixer_set_source_open(&mixer, 0, true);
    atomic_store(&g_fed_frames, 0);
    assert(native_mixer_pump_start(&mixer, counting_feed, NULL));

    int16_t burst[96 * 2];
    memset(burst, 0x22, sizeof(burst));
    assert(native_mixer_push(&mixer, 0, burst, 96) == 0); /* 2ms < 10ms prebuffer */

    for (int i = 0; i < 3000 && atomic_load(&g_fed_frames) < 96; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    assert(atomic_load(&g_fed_frames) >= 96);
    native_mixer_pump_stop(&mixer);
    native_mixer_destroy(&mixer);
}

/* Audio queued while nothing consumed it is already-late: a starting pump must trim each
 * ring to the jitter target (prebuffer + one chunk) instead of replaying the backlog as a
 * permanent A/V offset. */
static void test_pump_start_trims_backlog(void) {
    NativeAudioMixer mixer;
    /* chunk 48, prebuffer 48 -> trim target 96 frames. */
    assert(native_mixer_init(&mixer, 48000, 2, 48, 4800, 48));
    native_mixer_set_source_open(&mixer, 0, true);

    int16_t data[4800 * 2];
    memset(data, 0x33, sizeof(data));
    assert(native_mixer_push(&mixer, 0, data, 4800) == 0); /* 100ms of backlog */

    atomic_store(&g_fed_frames, 0);
    assert(native_mixer_pump_start(&mixer, counting_feed, NULL));
    /* Only the trimmed remainder may come out; wait for it, then confirm nothing more. */
    for (int i = 0; i < 2000 && atomic_load(&g_fed_frames) < 96; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    struct timespec settle = {0, 50000000}; /* 50ms: pump would keep feeding a backlog */
    nanosleep(&settle, NULL);
    assert(atomic_load(&g_fed_frames) == 96);
    native_mixer_pump_stop(&mixer);
    native_mixer_destroy(&mixer);
}

/* Idle silence must never queue in front of real audio: while a sub-prebuffer burst
 * waits for stale promotion, an armed idle window must feed NOTHING (silence fed then
 * would sit in the hardware queue ahead of the sound as invisible latency). */
static atomic_uint g_silence_before_real;
static atomic_bool g_real_seen;

static void silence_order_feed(void *ctx, const int16_t *samples, size_t frames) {
    bool is_silence = true;
    for (size_t i = 0; i < frames && is_silence; i++) {
        is_silence = samples[i * 2] == 0 && samples[i * 2 + 1] == 0;
    }
    if (!is_silence) {
        atomic_store(&g_real_seen, true);
    } else if (!atomic_load(&g_real_seen)) {
        atomic_fetch_add(&g_silence_before_real, 1u);
    }
    counting_feed(ctx, samples, frames);
}

static void test_idle_silence_never_precedes_queued_audio(void) {
    NativeAudioMixer mixer;
    /* prebuffer 4800 = 100ms: the 96-frame burst stays sub-prebuffer until the stale
     * flush (~100ms) promotes it; chunk 48 = 1ms cadence would emit dozens of silence
     * chunks in that window if the guard were missing. */
    assert(native_mixer_init(&mixer, 48000, 2, 48, 48000, 4800));
    native_mixer_set_source_open(&mixer, 0, true);
    atomic_store(&g_fed_frames, 0);
    atomic_store(&g_silence_before_real, 0);
    atomic_store(&g_real_seen, false);

    int16_t burst[96 * 2];
    memset(burst, 0x77, sizeof(burst));
    assert(native_mixer_push(&mixer, 0, burst, 96) == 0);
    native_mixer_feed_silence_for(&mixer, 1000);
    assert(native_mixer_pump_start(&mixer, silence_order_feed, NULL));

    for (int i = 0; i < 3000 && !atomic_load(&g_real_seen); i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    assert(atomic_load(&g_real_seen));
    assert(atomic_load(&g_silence_before_real) == 0);
    native_mixer_pump_stop(&mixer);
    native_mixer_destroy(&mixer);
}

/* A ring floor that stays above the jitter target for a whole stats window is standing
 * backlog (gradual accumulation the cadence-lateness check cannot see); the flush must
 * trim it down to the target. */
static void test_flush_trims_standing_backlog(void) {
    NativeAudioMixer mixer;
    /* chunk 48, prebuffer 48 -> target 96 frames. */
    assert(native_mixer_init(&mixer, 48000, 2, 48, 48000, 48));
    native_mixer_set_source_open(&mixer, 0, true);

    /* Floor must clear the trim hysteresis (target + 50ms = 96 + 2400 frames). */
    int16_t data[12000 * 2];
    memset(data, 0x66, sizeof(data));
    assert(native_mixer_push(&mixer, 0, data, 12000) == 0);
    int16_t out[48 * 2];
    for (int i = 0; i < 3; i++) {
        assert(native_mixer_pull(&mixer, out)); /* window floor settles at 11856 frames */
    }

    native_mixer_flush_stats(&mixer);

    /* Exactly the target survives the trim: two more chunks, then dry. */
    int chunks = 0;
    while (native_mixer_ready(&mixer) && chunks < 100) {
        assert(native_mixer_pull(&mixer, out));
        chunks++;
    }
    assert(chunks == 2);

    /* A floor at/below the target must NOT be touched. */
    assert(native_mixer_push(&mixer, 0, data, 96) == 0);
    assert(native_mixer_pull(&mixer, out)); /* floor 48 < target */
    native_mixer_flush_stats(&mixer);
    assert(native_mixer_ready(&mixer));
    assert(native_mixer_pull(&mixer, out));
    assert(!native_mixer_ready(&mixer));

    native_mixer_destroy(&mixer);
}

/* A feed that blocks (a shared-pipeline reload holds the sink's lock) while audio keeps
 * arriving must not bake the accumulated backlog in as permanent latency: the cadence
 * anchors BEFORE the feed, so the stall is visible to the lateness check and trimmed. */
static atomic_bool g_block_first_feed;

static void blocking_first_feed(void *ctx, const int16_t *samples, size_t frames) {
    if (atomic_exchange(&g_block_first_feed, false)) {
        struct timespec ts = {0, 320000000}; /* well past the 250ms stall threshold */
        nanosleep(&ts, NULL);
    }
    counting_feed(ctx, samples, frames);
}

static void test_blocked_feed_does_not_bake_in_backlog(void) {
    NativeAudioMixer mixer;
    /* chunk 48 = 1ms @48k, prebuffer 0 -> trim target 48 frames. */
    assert(native_mixer_init(&mixer, 48000, 2, 48, 48000, 0));
    native_mixer_set_source_open(&mixer, 0, true);
    atomic_store(&g_fed_frames, 0);
    atomic_store(&g_block_first_feed, true);

    int16_t chunk[48 * 2];
    memset(chunk, 0x44, sizeof(chunk));
    assert(native_mixer_push(&mixer, 0, chunk, 48) == 0);
    assert(native_mixer_pump_start(&mixer, blocking_first_feed, NULL));

    /* Let the pump enter the blocking feed, then pile up ~300ms of "arriving" audio. */
    struct timespec settle = {0, 50000000};
    nanosleep(&settle, NULL);
    int16_t backlog[4800 * 2];
    memset(backlog, 0x55, sizeof(backlog));
    for (int i = 0; i < 3; i++) {
        assert(native_mixer_push(&mixer, 0, backlog, 4800) == 0);
    }

    /* After the stall the trim keeps one target's worth (48): total fed = the blocked
     * chunk + the trimmed remainder, NOT the 14.4k-frame backlog. */
    for (int i = 0; i < 2000 && atomic_load(&g_fed_frames) < 96; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    nanosleep(&settle, NULL);
    assert(atomic_load(&g_fed_frames) == 96);
    native_mixer_pump_stop(&mixer);
    native_mixer_destroy(&mixer);
}

/* Two rdp-workers re-pinning the mix format may stop the pump concurrently; exactly one
 * join must happen (control_lock) and the second call must be a clean no-op. */
static void *stop_thread_main(void *arg) {
    native_mixer_pump_stop((NativeAudioMixer *)arg);
    return NULL;
}

static void test_pump_stop_is_safe_concurrently(void) {
    NativeAudioMixer mixer;
    assert(native_mixer_init(&mixer, 48000, 2, 48, 480, 48));
    atomic_store(&g_fed_frames, 0);
    assert(native_mixer_pump_start(&mixer, counting_feed, NULL));

    pthread_t stoppers[3];
    for (int i = 0; i < 3; i++) {
        assert(pthread_create(&stoppers[i], NULL, stop_thread_main, &mixer) == 0);
    }
    for (int i = 0; i < 3; i++) {
        assert(pthread_join(stoppers[i], NULL) == 0);
    }
    assert(!mixer.thread_running);
    /* The pump can start again after the concurrent stop settled. */
    assert(native_mixer_pump_start(&mixer, counting_feed, NULL));
    native_mixer_pump_stop(&mixer);
    native_mixer_destroy(&mixer);
}

int main(void) {
    test_init_validation();
    test_closed_source_drops_pushes();
    test_single_source_identity();
    test_two_sources_are_summed();
    test_three_sources_clamp_once();
    test_stale_burst_promoted_while_other_source_plays();
    test_sum_saturates();
    test_prebuffer_gates_contribution();
    test_underrun_pads_silence_and_reprimes();
    test_partial_final_chunk_pads_with_silence();
    test_capacity_overflow_drops_oldest();
    test_close_clears_ring();
    test_source_gain_scales_and_survives_reopen();
    test_zero_prebuffer_is_immediate();
    test_pump_delivers_and_stops();
    test_pump_flushes_stale_sub_prebuffer_burst();
    test_pump_start_trims_backlog();
    test_flush_trims_standing_backlog();
    test_idle_silence_never_precedes_queued_audio();
    test_blocked_feed_does_not_bake_in_backlog();
    test_pump_stop_is_safe_concurrently();
    printf("test_audio_mixer: all tests passed\n");
    return 0;
}
