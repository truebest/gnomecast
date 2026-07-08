#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "audio_mixer.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool native_mixer_init(NativeAudioMixer *mixer, uint32_t sample_rate, uint16_t channels, size_t chunk_frames,
                       size_t capacity_frames, size_t prebuffer_frames) {
    if (!mixer || sample_rate == 0 || channels == 0 || chunk_frames == 0 || capacity_frames < chunk_frames ||
        prebuffer_frames > capacity_frames) {
        return false;
    }
    memset(mixer, 0, sizeof(*mixer));
    if (pthread_mutex_init(&mixer->lock, NULL) != 0) {
        return false;
    }
    if (pthread_mutex_init(&mixer->control_lock, NULL) != 0) {
        pthread_mutex_destroy(&mixer->lock);
        return false;
    }
    pthread_condattr_t cond_attr;
    bool cond_ok = pthread_condattr_init(&cond_attr) == 0;
    if (cond_ok) {
        /* The pump's stale-burst timeout waits on CLOCK_MONOTONIC deadlines. */
        cond_ok = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0 &&
                  pthread_cond_init(&mixer->cond, &cond_attr) == 0;
        pthread_condattr_destroy(&cond_attr);
    }
    if (!cond_ok) {
        pthread_mutex_destroy(&mixer->control_lock);
        pthread_mutex_destroy(&mixer->lock);
        return false;
    }
    mixer->sample_rate = sample_rate;
    mixer->channels = channels;
    mixer->chunk_frames = chunk_frames;
    mixer->capacity_frames = capacity_frames;
    mixer->prebuffer_frames = prebuffer_frames;
    mixer->stale_flush_ns = (uint64_t)prebuffer_frames * 1000000000u / sample_rate;
    if (mixer->stale_flush_ns < 100000000u) {
        mixer->stale_flush_ns = 100000000u; /* floor keeps idle wakeups cheap */
    }
    mixer->mix_buf = (int16_t *)calloc(chunk_frames * channels, sizeof(int16_t));
    mixer->accum_buf = (int32_t *)calloc(chunk_frames * channels, sizeof(int32_t));
    if (!mixer->mix_buf || !mixer->accum_buf) {
        free(mixer->mix_buf);
        free(mixer->accum_buf);
        pthread_cond_destroy(&mixer->cond);
        pthread_mutex_destroy(&mixer->control_lock);
        pthread_mutex_destroy(&mixer->lock);
        memset(mixer, 0, sizeof(*mixer));
        return false;
    }
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        mixer->sources[i].ring = (int16_t *)calloc(capacity_frames * channels, sizeof(int16_t));
        if (!mixer->sources[i].ring) {
            for (int j = 0; j < i; j++) {
                free(mixer->sources[j].ring);
            }
            free(mixer->mix_buf);
            free(mixer->accum_buf);
            pthread_cond_destroy(&mixer->cond);
            pthread_mutex_destroy(&mixer->control_lock);
            pthread_mutex_destroy(&mixer->lock);
            memset(mixer, 0, sizeof(*mixer));
            return false;
        }
        mixer->sources[i].gain_q15 = NATIVE_MIXER_GAIN_UNITY_Q15; /* the zeroed default would be silence */
    }
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        mixer->sources[i].stat_min_len = SIZE_MAX;
    }
    mixer->initialized = true;
    return true;
}

void native_mixer_destroy(NativeAudioMixer *mixer) {
    if (!mixer || !mixer->initialized) {
        return;
    }
    native_mixer_pump_stop(mixer);
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        free(mixer->sources[i].ring);
    }
    free(mixer->mix_buf);
    free(mixer->accum_buf);
    pthread_cond_destroy(&mixer->cond);
    pthread_mutex_destroy(&mixer->control_lock);
    pthread_mutex_destroy(&mixer->lock);
    memset(mixer, 0, sizeof(*mixer));
}

static bool source_index_valid(int source) {
    return source >= 0 && source < NATIVE_MIXER_MAX_SOURCES;
}

/* a - b in nanoseconds, normalizing the borrow across a second rollover; the caller
 * guarantees a >= b (CLOCK_MONOTONIC pairs). */
static uint64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b) {
    time_t sec = a->tv_sec - b->tv_sec;
    long nsec = a->tv_nsec - b->tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }
    return (uint64_t)sec * 1000000000u + (uint64_t)nsec;
}

void native_mixer_set_source_open(NativeAudioMixer *mixer, int source, bool open) {
    if (!mixer || !mixer->initialized || !source_index_valid(source)) {
        return;
    }
    pthread_mutex_lock(&mixer->lock);
    NativeMixerSource *src = &mixer->sources[source];
    src->open = open;
    src->live = false;
    src->start_frames = 0;
    src->len_frames = 0;
    src->queue_aging = false;
    src->drop_logged = false;
    src->reprime_logged = false;
    src->stat_min_len = SIZE_MAX;
    src->stat_max_len = 0;
    src->stat_max_push = 0;
    src->stat_pushes = 0;
    src->peak_left = 0;
    src->peak_right = 0;
    pthread_mutex_unlock(&mixer->lock);
}

void native_mixer_set_source_gain(NativeAudioMixer *mixer, int source, int32_t gain_q15) {
    if (!mixer || !mixer->initialized || !source_index_valid(source)) {
        return;
    }
    if (gain_q15 < 0) {
        gain_q15 = 0;
    }
    if (gain_q15 > NATIVE_MIXER_GAIN_MAX_Q15) {
        gain_q15 = NATIVE_MIXER_GAIN_MAX_Q15;
    }
    pthread_mutex_lock(&mixer->lock);
    mixer->sources[source].gain_q15 = gain_q15;
    pthread_mutex_unlock(&mixer->lock);
}

void native_mixer_get_source_peaks(NativeAudioMixer *mixer, int source, int32_t *left, int32_t *right) {
    if (left) {
        *left = 0;
    }
    if (right) {
        *right = 0;
    }
    if (!mixer || !mixer->initialized || !source_index_valid(source)) {
        return;
    }
    pthread_mutex_lock(&mixer->lock);
    NativeMixerSource *src = &mixer->sources[source];
    if (src->peak_left != 0 || src->peak_right != 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t age_ns = timespec_diff_ns(&now, &src->peak_when);
        if (age_ns <= 100000000u) { /* a few chunk periods; older = the pump paused */
            if (left) {
                *left = src->peak_left;
            }
            if (right) {
                *right = src->peak_right;
            }
        }
    }
    pthread_mutex_unlock(&mixer->lock);
}

/* Caller holds the lock. */
static void source_write_frames(NativeAudioMixer *mixer, NativeMixerSource *src, const int16_t *samples,
                                size_t frames) {
    size_t channels = mixer->channels;
    size_t write_pos = (src->start_frames + src->len_frames) % mixer->capacity_frames;
    size_t remaining = frames;
    while (remaining > 0) {
        size_t contiguous = mixer->capacity_frames - write_pos;
        size_t batch = remaining < contiguous ? remaining : contiguous;
        memcpy(&src->ring[write_pos * channels], samples, batch * channels * sizeof(int16_t));
        samples += batch * channels;
        write_pos = (write_pos + batch) % mixer->capacity_frames;
        remaining -= batch;
    }
    src->len_frames += frames;
}

size_t native_mixer_push(NativeAudioMixer *mixer, int source, const int16_t *samples, size_t frames) {
    if (!mixer || !mixer->initialized || !source_index_valid(source) || !samples || frames == 0) {
        return 0;
    }
    pthread_mutex_lock(&mixer->lock);
    NativeMixerSource *src = &mixer->sources[source];
    if (!src->open) {
        pthread_mutex_unlock(&mixer->lock);
        return 0;
    }

    /* The ring capacity IS the latency cap: a source can never fall further behind than
     * capacity_frames (~1.5s). Overflow drops the OLDEST audio — one audible seam, and
     * only when the producer genuinely outruns real time (e.g. a stream-restart dump). */
    if (!src->live && src->len_frames == 0) {
        /* Data starts queueing while (re)priming: age it so a sub-prebuffer burst is
         * promoted after stale_flush_ns even if other sources keep the pump busy. */
        clock_gettime(CLOCK_MONOTONIC, &src->queued_since);
        src->queue_aging = true;
    }

    size_t dropped = 0;
    if (frames >= mixer->capacity_frames) {
        /* Pathological oversize push: keep only the newest capacity's worth. */
        dropped = src->len_frames + frames - mixer->capacity_frames;
        samples += (frames - mixer->capacity_frames) * mixer->channels;
        frames = mixer->capacity_frames;
        src->start_frames = 0;
        src->len_frames = 0;
    } else if (src->len_frames + frames > mixer->capacity_frames) {
        dropped = src->len_frames + frames - mixer->capacity_frames;
        src->start_frames = (src->start_frames + dropped) % mixer->capacity_frames;
        src->len_frames -= dropped;
    }
    source_write_frames(mixer, src, samples, frames);
    if (frames > src->stat_max_push) {
        src->stat_max_push = frames;
    }
    if (src->len_frames > src->stat_max_len) {
        src->stat_max_len = src->len_frames;
    }
    src->stat_pushes++;

    if (dropped > 0 && !src->drop_logged) {
        fprintf(stderr, "[native-mixer] source %d ring full; dropped %zu oldest frames (consumer behind)\n", source,
                dropped);
        src->drop_logged = true;
    } else if (dropped == 0) {
        src->drop_logged = false;
    }

    pthread_cond_signal(&mixer->cond);
    pthread_mutex_unlock(&mixer->lock);
    return dropped;
}

/* A feed stall must be at least this long before the pump discards ring backlog:
 * scheduling hiccups stay untouched, real outages (a shared-pipeline reload can hold the
 * sink's lock for over a second) get their latency reset. */
#define NATIVE_MIXER_STALL_TRIM_NS 250000000u

/* Caller holds the lock. Drops each open source's OLDEST queued audio down to the jitter
 * target (prebuffer + one chunk). Runs when consumption resumes after an outage: the
 * excess is audio the user already sat through in silence, and replaying it late would
 * become a PERMANENT A/V offset — producers and the pump both run at real time, so ring
 * backlog never drains on its own. */
static void mixer_trim_backlog_locked(NativeAudioMixer *mixer, const char *reason) {
    size_t target = mixer->prebuffer_frames + mixer->chunk_frames;
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        NativeMixerSource *src = &mixer->sources[i];
        if (!src->open || src->len_frames <= target) {
            continue;
        }
        size_t drop = src->len_frames - target;
        src->start_frames = (src->start_frames + drop) % mixer->capacity_frames;
        src->len_frames = target;
        fprintf(stderr, "[native-mixer] source %d: dropped %zu backlog frames after %s (latency reset)\n", i, drop,
                reason);
    }
}

/* Caller holds the lock. Promotes (re)priming sources whose queued burst is older than
 * the stale threshold: without this, a short sound from a quiet session would sit below
 * the prebuffer level indefinitely while other sessions keep contributing. */
static void mixer_promote_stale_locked(NativeAudioMixer *mixer) {
    struct timespec now;
    bool have_now = false;
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        NativeMixerSource *src = &mixer->sources[i];
        if (!src->open || src->live || !src->queue_aging || src->len_frames == 0) {
            continue;
        }
        if (!have_now) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            have_now = true;
        }
        uint64_t age_ns = timespec_diff_ns(&now, &src->queued_since);
        if (age_ns >= mixer->stale_flush_ns) {
            src->live = true; /* stale sub-prebuffer burst: drain it now */
            src->queue_aging = false;
        }
    }
}

/* Caller holds the lock. A source can contribute once it is live with queued data, or has
 * finished (re)priming its jitter buffer. */
static bool source_can_contribute(const NativeAudioMixer *mixer, const NativeMixerSource *src) {
    if (!src->open || src->len_frames == 0) {
        return false;
    }
    return src->live || src->len_frames >= mixer->prebuffer_frames;
}

static bool mixer_ready_locked(const NativeAudioMixer *mixer) {
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        if (source_can_contribute(mixer, &mixer->sources[i])) {
            return true;
        }
    }
    return false;
}

/* Caller holds the lock. Real audio exists somewhere (possibly still sub-prebuffer). */
static bool mixer_any_queued_locked(const NativeAudioMixer *mixer) {
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        if (mixer->sources[i].open && mixer->sources[i].len_frames > 0) {
            return true;
        }
    }
    return false;
}

bool native_mixer_ready(NativeAudioMixer *mixer) {
    if (!mixer || !mixer->initialized) {
        return false;
    }
    pthread_mutex_lock(&mixer->lock);
    bool ready = mixer_ready_locked(mixer);
    pthread_mutex_unlock(&mixer->lock);
    return ready;
}

static int16_t saturate_i16(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

/* Caller holds the lock. */
static bool mixer_pull_locked(NativeAudioMixer *mixer, int16_t *out) {
    mixer_promote_stale_locked(mixer);
    if (!mixer_ready_locked(mixer)) {
        return false;
    }
    size_t channels = mixer->channels;
    size_t out_samples = mixer->chunk_frames * channels;
    /* Sum every source into the wide accumulator and clamp ONCE at the end: clipping
     * after each addition would corrupt the mix as soon as a third source cancels an
     * earlier saturated peak (30000 + 30000 - 30000 must stay 30000, not 2767). */
    memset(mixer->accum_buf, 0, out_samples * sizeof(int32_t));

    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        NativeMixerSource *src = &mixer->sources[i];
        if (source_can_contribute(mixer, src)) {
            src->live = true;
            src->queue_aging = false;
        }
        if (!src->live || src->len_frames == 0) {
            if (src->live) {
                src->live = false; /* ran completely dry between pulls: re-arm the prebuffer */
            }
            /* Silent this chunk: the meter must fall instead of holding the last burst. */
            src->peak_left = 0;
            src->peak_right = 0;
            continue;
        }

        size_t take = src->len_frames < mixer->chunk_frames ? src->len_frames : mixer->chunk_frames;
        int32_t gain_q15 = src->gain_q15;
        int32_t peak[2] = {0, 0};
        for (size_t frame = 0; frame < take; frame++) {
            size_t ring_frame = (src->start_frames + frame) % mixer->capacity_frames;
            const int16_t *in = &src->ring[ring_frame * channels];
            int32_t *acc = &mixer->accum_buf[frame * channels];
            for (size_t ch = 0; ch < channels; ch++) {
                /* Division, not >>: well-defined for negative samples, and 32768/32768
                 * keeps unity bit-exact. Max magnitude 32767*65536 fits int32. */
                int32_t sample = (int32_t)in[ch] * gain_q15 / 32768;
                acc[ch] += sample;
                int32_t magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak[ch & 1]) {
                    peak[ch & 1] = magnitude;
                }
            }
        }
        src->peak_left = peak[0];
        src->peak_right = channels > 1 ? peak[1] : peak[0];
        clock_gettime(CLOCK_MONOTONIC, &src->peak_when);
        src->start_frames = (src->start_frames + take) % mixer->capacity_frames;
        src->len_frames -= take;

        if (take < mixer->chunk_frames || src->len_frames == 0) {
            /* Underrun: this source pauses (shortfall already played as silence) and must
             * re-fill its jitter margin before contributing again. */
            src->live = false;
            if (!src->reprime_logged) {
                fprintf(stderr, "[native-mixer] source %d ran dry; re-priming %zu frames\n", i,
                        mixer->prebuffer_frames);
                src->reprime_logged = true;
            }
        } else {
            src->reprime_logged = false;
        }
    }

    for (size_t sample = 0; sample < out_samples; sample++) {
        out[sample] = saturate_i16(mixer->accum_buf[sample]);
    }
    /* Post-take depth is the honest standing level: its window minimum (the floor) is
     * audio that persistently sat unplayed — the input to the standing-backlog trim. */
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        NativeMixerSource *src = &mixer->sources[i];
        if (src->open && src->len_frames < src->stat_min_len) {
            src->stat_min_len = src->len_frames;
        }
    }
    return true;
}

bool native_mixer_pull(NativeAudioMixer *mixer, int16_t *out) {
    if (!mixer || !mixer->initialized || !out) {
        return false;
    }
    pthread_mutex_lock(&mixer->lock);
    bool produced = mixer_pull_locked(mixer, out);
    pthread_mutex_unlock(&mixer->lock);
    return produced;
}

void native_mixer_feed_silence_for(NativeAudioMixer *mixer, uint32_t ms) {
    if (!mixer || !mixer->initialized) {
        return;
    }
    pthread_mutex_lock(&mixer->lock);
    clock_gettime(CLOCK_MONOTONIC, &mixer->idle_feed_until);
    mixer->idle_feed_until.tv_sec += ms / 1000u;
    mixer->idle_feed_until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (mixer->idle_feed_until.tv_nsec >= 1000000000L) {
        mixer->idle_feed_until.tv_nsec -= 1000000000L;
        mixer->idle_feed_until.tv_sec += 1;
    }
    mixer->idle_feed_armed = true;
    pthread_cond_signal(&mixer->cond);
    pthread_mutex_unlock(&mixer->lock);
}

static void timespec_add_ns(struct timespec *ts, uint64_t ns) {
    ts->tv_nsec += (long)(ns % 1000000000u);
    ts->tv_sec += (time_t)(ns / 1000000000u);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

static bool timespec_before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

/* Caller holds the lock. One line per open source every stats period; the numbers place
 * the audio latency: depth floor = audio held client-side, max push = server batching.
 *
 * The floor also DRIVES latency recovery: a floor above the jitter target for a whole
 * period is standing backlog by construction (every pull in the window left that much
 * behind; bursts only raise the ceiling), so the excess is dropped. This catches what
 * the cadence-lateness check cannot: gradual accumulation in sub-threshold slices, e.g.
 * the sink dribble-accepting audio slower than real time until NDL reaches PLAYING —
 * observed live as a permanent ~570ms ring floor with no single stall to blame. */
static void mixer_log_stats_locked(NativeAudioMixer *mixer) {
    size_t target = mixer->prebuffer_frames + mixer->chunk_frames;
    /* Hysteresis: act only when the floor exceeds the target by >50ms. Below that, slow
     * clock drift rides free; above it, one ~50ms correction — observed live: a single
     * jitter spike otherwise parks an extra 50-70ms of standing latency just under the
     * old 100ms threshold, forever. */
    size_t trim_threshold = target + mixer->sample_rate / 20u;
    for (int i = 0; i < NATIVE_MIXER_MAX_SOURCES; i++) {
        NativeMixerSource *src = &mixer->sources[i];
        if (!src->open) {
            continue;
        }
        if (src->stat_min_len != SIZE_MAX && src->stat_min_len > trim_threshold &&
            src->len_frames >= src->stat_min_len) {
            size_t drop = src->stat_min_len - target;
            src->start_frames = (src->start_frames + drop) % mixer->capacity_frames;
            src->len_frames -= drop;
            fprintf(stderr,
                    "[native-mixer] source %d: standing backlog, floor %ums above the %ums target; dropped %zu "
                    "frames (latency reset)\n",
                    i, (unsigned)(src->stat_min_len * 1000u / mixer->sample_rate),
                    (unsigned)(target * 1000u / mixer->sample_rate), drop);
        }
        if (src->stat_pushes > 0 || src->stat_max_len > 0) {
            size_t min_len = src->stat_min_len == SIZE_MAX ? 0 : src->stat_min_len;
            fprintf(stderr,
                    "[native-mixer] stats src %d: ring %zu..%zu frames (%u..%ums), max push %zu (%ums), %u pushes\n",
                    i, min_len, src->stat_max_len, (unsigned)(min_len * 1000u / mixer->sample_rate),
                    (unsigned)(src->stat_max_len * 1000u / mixer->sample_rate), src->stat_max_push,
                    (unsigned)(src->stat_max_push * 1000u / mixer->sample_rate), src->stat_pushes);
        }
        src->stat_min_len = SIZE_MAX;
        src->stat_max_len = 0;
        src->stat_max_push = 0;
        src->stat_pushes = 0;
    }
}

void native_mixer_flush_stats(NativeAudioMixer *mixer) {
    if (!mixer || !mixer->initialized) {
        return;
    }
    pthread_mutex_lock(&mixer->lock);
    mixer_log_stats_locked(mixer);
    pthread_mutex_unlock(&mixer->lock);
}

#define NATIVE_MIXER_STATS_PERIOD_NS 3000000000u

static void *mixer_pump_main(void *arg) {
    NativeAudioMixer *mixer = (NativeAudioMixer *)arg;
    const uint64_t chunk_ns = (uint64_t)mixer->chunk_frames * 1000000000u / mixer->sample_rate;
    struct timespec next_tick;
    struct timespec next_stats;
    bool anchored = false;
    clock_gettime(CLOCK_MONOTONIC, &next_stats);
    timespec_add_ns(&next_stats, NATIVE_MIXER_STATS_PERIOD_NS);

    pthread_mutex_lock(&mixer->lock);
    while (!mixer->stop_requested) {
        mixer_promote_stale_locked(mixer);
        if (!mixer_ready_locked(mixer)) {
            /* Idle-feed window (fresh sink track): emit paced silence instead of pausing
             * so NDL completes its LOADCOMPLETED->PLAYING transition. */
            bool idle_feed = false;
            if (mixer->idle_feed_armed) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (!timespec_before(&now, &mixer->idle_feed_until)) {
                    mixer->idle_feed_armed = false;
                } else if (!mixer_any_queued_locked(mixer)) {
                    /* Feed silence only while NO real audio is queued anywhere: until
                     * PLAYING the sink drains slower than real time, so silence fed
                     * ahead of a priming source would sit in the hardware queue in
                     * front of the actual sound as latency the ring cannot see. A
                     * sub-prebuffer burst reaches the mix via stale promotion instead. */
                    idle_feed = true;
                }
            }
            if (idle_feed) {
                memset(mixer->mix_buf, 0, mixer->chunk_frames * (size_t)mixer->channels * sizeof(int16_t));
            } else {
                /* All sources silent: stop feeding so the sink drains naturally, and
                 * re-anchor the cadence when data returns. */
                anchored = false;
                struct timespec deadline;
                clock_gettime(CLOCK_MONOTONIC, &deadline);
                timespec_add_ns(&deadline, mixer->stale_flush_ns);
                int rc = pthread_cond_timedwait(&mixer->cond, &mixer->lock, &deadline);
                if (rc == ETIMEDOUT) {
                    mixer_promote_stale_locked(mixer);
                }
                continue;
            }
        } else {
            (void)mixer_pull_locked(mixer, mixer->mix_buf);
        }
        pthread_mutex_unlock(&mixer->lock);

        /* Anchor the cadence BEFORE the feed: the feed can block for the whole duration
         * of a shared-pipeline (re)load (its callback takes the sink's lock), and
         * anchoring afterwards would hide exactly that stall from the lateness check
         * below — the rings would keep the accumulated backlog as a permanent A/V
         * offset (observed live: ~600ms baked in at every session start). */
        if (!anchored) {
            clock_gettime(CLOCK_MONOTONIC, &next_tick);
            anchored = true;
        }

        mixer->feed(mixer->feed_ctx, mixer->mix_buf, mixer->chunk_frames);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!timespec_before(&now, &next_stats)) {
            native_mixer_flush_stats(mixer);
            next_stats = now;
            timespec_add_ns(&next_stats, NATIVE_MIXER_STATS_PERIOD_NS);
        }
        timespec_add_ns(&next_tick, chunk_ns);
        if (timespec_before(&next_tick, &now)) {
            /* Fell badly behind (a slow feed, scheduling stall): re-anchor instead of
             * bursting to catch up. Past the stall threshold, also discard what the
             * rings accumulated meanwhile — that audio's playback slot is gone, and
             * keeping it would turn the outage into a permanent A/V offset. */
            uint64_t late_ns = timespec_diff_ns(&now, &next_tick);
            next_tick = now;
            if (late_ns >= NATIVE_MIXER_STALL_TRIM_NS) {
                pthread_mutex_lock(&mixer->lock);
                mixer_trim_backlog_locked(mixer, "a feed stall");
                pthread_mutex_unlock(&mixer->lock);
            }
        } else {
            int rc;
            do {
                rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
            } while (rc == EINTR);
        }
        pthread_mutex_lock(&mixer->lock);
    }
    pthread_mutex_unlock(&mixer->lock);
    return NULL;
}

bool native_mixer_pump_start(NativeAudioMixer *mixer, void (*feed)(void *ctx, const int16_t *samples, size_t frames),
                             void *feed_ctx) {
    if (!mixer || !mixer->initialized || !feed) {
        return false;
    }
    pthread_mutex_lock(&mixer->control_lock);
    if (mixer->thread_running) {
        pthread_mutex_unlock(&mixer->control_lock);
        return false;
    }
    /* Anything queued while no pump was consuming is already-late audio. */
    pthread_mutex_lock(&mixer->lock);
    mixer_trim_backlog_locked(mixer, "a pump (re)start");
    pthread_mutex_unlock(&mixer->lock);
    mixer->feed = feed;
    mixer->feed_ctx = feed_ctx;
    mixer->stop_requested = false;
    if (pthread_create(&mixer->thread, NULL, mixer_pump_main, mixer) != 0) {
        fprintf(stderr, "[native-mixer] failed to start the mixer pump thread\n");
        pthread_mutex_unlock(&mixer->control_lock);
        return false;
    }
    mixer->thread_running = true;
    pthread_mutex_unlock(&mixer->control_lock);
    return true;
}

void native_mixer_pump_stop(NativeAudioMixer *mixer) {
    if (!mixer || !mixer->initialized) {
        return;
    }
    /* control_lock makes concurrent stops safe: rdp-workers re-pinning the mix format at
     * the same time may both get here, and pthread_join must run exactly once. The pump
     * thread never takes control_lock, so holding it across the join cannot deadlock. */
    pthread_mutex_lock(&mixer->control_lock);
    if (!mixer->thread_running) {
        pthread_mutex_unlock(&mixer->control_lock);
        return;
    }
    pthread_mutex_lock(&mixer->lock);
    mixer->stop_requested = true;
    pthread_cond_signal(&mixer->cond);
    pthread_mutex_unlock(&mixer->lock);
    pthread_join(mixer->thread, NULL);
    mixer->thread_running = false;
    pthread_mutex_unlock(&mixer->control_lock);
}
