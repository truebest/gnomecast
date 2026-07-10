# Future work

This document records ideas that are intentionally deferred. They are not scheduled
features and should not change the current behavior until their implementation and
device acceptance criteria are complete.

## Isolate the NDL audio sink from the miniaudio pump

Status: **deferred**. Keep the current NDL/ss4s sink and F32 miniaudio graph for now.

### Motivation

The graph pump currently renders one 480-frame/10ms block and then feeds NDL
synchronously. The feed shares application and media serialization with video. A long
video reload, lock hold, or NDL call therefore stops the pump from consuming all source
rings. RDP workers continue producing PCM, the source queues grow, and recovery can
require an otherwise unnecessary hard trim.

The goal is to contain a sink stall at the NDL boundary. Source jitter buffers should
continue being rendered at their normal cadence even while NDL is temporarily blocked.

### Proposed design

Introduce a `NativeAudioSink` actor with these ownership rules:

- The miniaudio pump only renders a block and submits it through a non-blocking,
  preallocated SPSC queue. Submission must not allocate, log, wait, or acquire a
  blocking mutex.
- A dedicated sink thread is the sole owner of `NativeAudio` and the only thread that
  calls `native_audio_open`, `native_audio_feed`, `native_audio_disable`, and
  `native_audio_close`.
- Rare `OPEN`, `RELOAD_IF_OPEN`, `CLOSE`, and `STOP` commands use a control path separate
  from PCM. The sink handles control before queued audio and fences each media generation
  so old PCM cannot cross a reload.
- Because audio and video share one `SS4S_Player`, `NativeMedia` gains an API gate used
  by every SS4S audio, video, viewport, and teardown call. The sink thread never acquires
  the application video-state lock, and the pump acquires neither lock.

Use eight fixed 10ms S16LE stereo blocks (about 15KiB) as the sink queue. Its 80ms
capacity is an emergency bound, not added working latency; steady-state depth should be
zero or one block. Keep the producer/consumer cursors single-owner and use only 32-bit
lock-free atomics on the ARMv7 callback path.

If the queue fills, the producer drops the new block and marks a discontinuity without
moving the consumer cursor. On a discontinuity, the consumer discards the stale queue.
It should also coalesce a queue at least 30ms deep to the newest block. Resume with a
5ms fade-in so recovery returns to live audio instead of playing an old burst or
clicking at an arbitrary sample phase.

NDL `NOT_READY` and overflow results drop the affected block and stale tail without a
tight retry. A fatal feed result disables audio once but does not close the shared track,
because an ss4s audio close unloads the video pipeline too. A later media reload may
reopen the sink. Audio remains best-effort throughout.

### Observability

Expose a throttled `NativeAudioSinkStats` snapshot containing:

- current queue depth and high-water mark;
- submitted, fed, full-drop, stale-drop, and closed-drop counts;
- NDL not-ready, overflow, and fatal counts;
- reload and flush counts;
- feed stalls of at least 20ms and maximum observed feed duration;
- current generation and open/disabled state.

The important diagnostic relationship is that sink stall/drop counters may rise while
the source queues remain near their adaptive targets and source hard-correction counters
do not rise because of the sink stall.

### Implementation outline

1. Add the shared `NativeMedia` API gate and make audio/video wrappers retain the media
   owner instead of an unprotected player pointer.
2. Add `audio_sink.c`/`.h`, its bounded SPSC data path, control path, lifecycle, and
   statistics.
3. Replace the pump's synchronous NDL callback with `native_audio_sink_submit()` and move
   all audio open/reload/disable/close operations onto the sink thread.
4. Update lifecycle ordering: start the sink before the pump; stop the pump before
   stopping and joining the sink; destroy media only after both have stopped.
5. Add host tests, run the normal native/Rust/CTest matrix, cross-build for webOS, and
   perform TV acceptance.

### Verification and acceptance

Deterministic tests should cover:

- all audio media calls execute on the same sink thread;
- FIFO steady state and a queue depth that never exceeds eight blocks;
- a blocked fake backend cannot block submit or grow source queues;
- overflow and a 30--40ms backlog recover with fresh PCM, not stale queued blocks;
- reload ordering is `feed return -> close -> open`, with no old-generation feed after
  the new open;
- NDL result handling, fatal disable, subsequent reload recovery, and clean shutdown;
- the submit path performs no allocation, logging, blocking mutex acquisition, or wait.

On the TV, repeat the 30-minute 4K/60 continuous-audio and multi-session acceptance run.
Induce or observe video/reload stalls and verify that sink telemetry records them while
source hard trims remain exceptional. Audio must recover at the live edge, video must
remain unaffected, and CPU/RSS must stay within the existing audio acceptance budgets.

### Risks and non-goals

- A permanently stuck NDL call cannot be cancelled safely while it may own platform
  state. The design isolates that failure from the graph and RDP workers, but lifecycle
  shutdown can still wait for the sink call to return.
- Verify unnamed semaphore support in the webOS cross-build; use `eventfd` as the wakeup
  fallback if necessary. Do not put a condition-variable mutex on the submit path.
- This work does not introduce an SDL audio device, a custom miniaudio NDL backend,
  WSOLA/PLC, or asynchronous video feeding.
