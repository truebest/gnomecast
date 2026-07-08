#ifndef GNOMECAST_AUDIO_MIXER_H
#define GNOMECAST_AUDIO_MIXER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* N-source S16LE PCM mixer for the multi-session audio path: every connected RDP session
 * decodes^W delivers PCM into its own ring; a pump thread pulls fixed-size chunks on a
 * wall-clock cadence, sums the sources with saturation and feeds the single shared ss4s
 * audio track. Video switching never touches this path, so background sessions stay
 * audible.
 *
 * Jitter model: each source carries its own prebuffer — it contributes to the mix only
 * once at least prebuffer_frames are queued (and re-arms whenever its ring runs dry), so
 * network jitter on one session inserts silence into that session alone instead of
 * stalling the whole mix. This replaces the downstream per-track prebuffer in
 * audio_ss4s.c for mixed audio (open the track with prebuffer 0).
 *
 * Clocking: the pump free-runs on CLOCK_MONOTONIC at chunk cadence while any source can
 * contribute, and blocks (feeding nothing, letting the hardware drain) once every ring is
 * silent. Long-term drift between the server clocks, this clock and the TV DAC resolves
 * as rare drop-oldest events (ring cap) or re-primes; both are logged and accepted.
 *
 * Latency model: rings never drain backlog on their own (producers and the pump both run
 * at real time), so audio accumulated while consumption stalled — a pump (re)start, or a
 * feed blocked >250ms behind cadence (shared-pipeline reloads hold the sink's lock for
 * up to seconds) — is trimmed back to prebuffer + one chunk. Without that, one transient
 * outage becomes a permanent A/V offset capped only by the ring capacity (~1.4s).
 *
 * Threading: push/set_source_open may be called from any thread (RDP workers); pull/ready
 * take the same internal lock. The feed callback runs on the pump thread WITHOUT the
 * mixer lock held, so it may take the app's video_lock; conversely, never call
 * native_mixer_pump_stop() while holding a lock the feed callback takes, or stop deadlocks
 * against an in-flight feed. */

/* Sized for the full remote-color-button set; unused sources cost one idle ring each. */
#define NATIVE_MIXER_MAX_SOURCES 4

/* Per-source gain range in Q15: 0 = mute, 32768 = unity, up to 2x unity (+6 dB boost).
 * The cap keeps the per-sample product inside int32 (32767 * 65536 < INT32_MAX). */
#define NATIVE_MIXER_GAIN_UNITY_Q15 32768
#define NATIVE_MIXER_GAIN_MAX_Q15 65536

typedef struct NativeMixerSource {
    int16_t *ring; /* capacity_frames * channels samples */
    size_t start_frames;
    size_t len_frames;
    bool open;
    bool live; /* false = (re)priming until prebuffer_frames are queued */
    /* Set when data started queueing while not live; a sub-prebuffer burst older than
     * the stale threshold is promoted to live even while OTHER sources keep the pump
     * busy, so short sounds are not held hostage until the next burst. */
    struct timespec queued_since;
    bool queue_aging;
    bool drop_logged;
    bool reprime_logged;
    /* Latency diagnostics, reset every stats period (lock-guarded): ring depth range
     * seen at pull time, largest single push, push count. The depth floor is the
     * client-held latency; the max push is the server's batching size. */
    size_t stat_min_len;
    size_t stat_max_len;
    size_t stat_max_push;
    unsigned stat_pushes;
    /* Q15 gain applied while summing this source into the mix (32768 = unity).
     * Deliberately NOT reset by open/close: a reconnecting session keeps its level. */
    int32_t gain_q15;
    /* Post-gain peak (max |sample|, pre-saturation, so up to ~2x INT16_MAX) of the last
     * pulled chunk, per stereo side; mono mirrors left into right. Stamped with the pull
     * time so a reader can ignore stale values once the pump pauses (see
     * native_mixer_get_source_peaks). Guarded by the mixer lock. */
    int32_t peak_left;
    int32_t peak_right;
    struct timespec peak_when;
} NativeMixerSource;

typedef struct NativeAudioMixer {
    pthread_mutex_t lock;
    /* Serializes pump_start/pump_stop against each other: rdp-workers may race two
     * pump_stops during concurrent format re-pins, and pthread_join must run exactly
     * once. Never taken by the pump thread itself, so holding it across the join is
     * safe. Ordering: never acquire `lock` while holding callers' outer locks — see the
     * feed-callback note below. */
    pthread_mutex_t control_lock;
    pthread_cond_t cond;
    bool initialized;
    uint32_t sample_rate;
    uint16_t channels;
    size_t chunk_frames;
    size_t capacity_frames;
    size_t prebuffer_frames;
    /* max(prebuffer duration, 100ms): how long a sub-prebuffer burst may wait before it
     * is played out padded with silence. */
    uint64_t stale_flush_ns;
    NativeMixerSource sources[NATIVE_MIXER_MAX_SOURCES];
    /* While set (deadline on CLOCK_MONOTONIC), the pump emits silence chunks at the
     * normal cadence when no source can contribute, instead of pausing — NDL defers a
     * fresh pipeline's LOADCOMPLETED->PLAYING until audio flows, so a track (re)opened
     * during an all-silent moment would stall the video start otherwise. */
    struct timespec idle_feed_until;
    bool idle_feed_armed;
    /* Pump thread state. */
    pthread_t thread;
    bool thread_running;
    bool stop_requested;
    void (*feed)(void *ctx, const int16_t *samples, size_t frames);
    void *feed_ctx;
    int16_t *mix_buf;  /* chunk_frames * channels samples */
    int32_t *accum_buf; /* wide mix accumulator: clamp once after ALL sources are summed */
} NativeAudioMixer;

/* Pure logic (host-tested; no pump thread involved). All functions no-op / fail cleanly on
 * a zeroed or destroyed mixer, so a memset(0) App field is safe to destroy unconditionally. */

bool native_mixer_init(NativeAudioMixer *mixer, uint32_t sample_rate, uint16_t channels, size_t chunk_frames,
                       size_t capacity_frames, size_t prebuffer_frames);
void native_mixer_destroy(NativeAudioMixer *mixer);

/* Opens/closes a source slot. Closing clears the ring; a closed source contributes nothing
 * and drops pushes. */
void native_mixer_set_source_open(NativeAudioMixer *mixer, int source, bool open);

/* Sets a source's Q15 gain (clamped to 0..NATIVE_MIXER_GAIN_MAX_Q15; unity is the init
 * default). Applied per sample while summing, before the saturating clamp; takes effect
 * on the next pulled chunk. A muted (gain 0) source still consumes its ring at cadence,
 * so un-muting is instant and stays in sync. Survives set_source_open (see gain_q15).
 * Callable from any thread. */
void native_mixer_set_source_gain(NativeAudioMixer *mixer, int source, int32_t gain_q15);

/* Post-gain peaks of the source's most recent pulled chunk, for volume metering (linear
 * |sample|, 0..~2x INT16_MAX; convert to dBFS against 32768). Returns zeros when the
 * source contributed nothing recently — including when the whole pump paused on silence,
 * where the last pulled peak would otherwise stay frozen (staleness cutoff 100ms, a few
 * chunk periods). Callable from any thread. */
void native_mixer_get_source_peaks(NativeAudioMixer *mixer, int source, int32_t *left, int32_t *right);

/* Queues interleaved S16LE frames from one session (RDP worker thread). When the ring is
 * full the OLDEST frames are dropped to keep latency bounded; returns the number of frames
 * dropped this call (0 = all fit). */
size_t native_mixer_push(NativeAudioMixer *mixer, int source, const int16_t *samples, size_t frames);

/* True when a pull would produce a chunk: some source is live with data queued, or has
 * finished (re)priming. */
bool native_mixer_ready(NativeAudioMixer *mixer);

/* Mixes one chunk_frames chunk into out (chunk_frames * channels samples): live sources
 * contribute min(queued, chunk) frames (shortfall = silence; running dry re-arms that
 * source's prebuffer), priming/closed sources contribute silence, everything is summed
 * with int16 saturation. Returns false (out untouched) when not ready. */
bool native_mixer_pull(NativeAudioMixer *mixer, int16_t *out);

/* Arms the idle silence feed for `ms` from now (see idle_feed_until). Callable from any
 * thread; typically right after the sink track is (re)opened. */
void native_mixer_feed_silence_for(NativeAudioMixer *mixer, uint32_t ms);

/* Logs per-source latency stats and trims standing backlog (a window floor above the
 * prebuffer + one chunk target) down to the target. The pump runs this on its own
 * cadence; exposed so tests can drive a window deterministically. */
void native_mixer_flush_stats(NativeAudioMixer *mixer);

/* Pump thread: feeds pulled chunks to `feed` at chunk cadence, pausing while no source is
 * ready. Returns false if the thread could not start (or is already running). */
bool native_mixer_pump_start(NativeAudioMixer *mixer, void (*feed)(void *ctx, const int16_t *samples, size_t frames),
                             void *feed_ctx);
/* Stops and joins the pump thread; safe to call when it never started and from multiple
 * threads at once (concurrent callers serialize; exactly one performs the join). */
void native_mixer_pump_stop(NativeAudioMixer *mixer);

#endif
