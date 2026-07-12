# Native Runbook

This runbook tracks the current native-only webOS target. The native app must not fall
back to a web app, JavaScript service, MSE, WebCodecs, RDCleanPath, or browser rendering.
AVC420/H.264 is preferred through the in-house NDL DirectMedia backend
(`backend_ndl`, dlopen of `libNDL_directmedia.so.1`); RemoteFX is supported only as a native
Rust/IronRDP decode path with RGBA updates presented by SDL.

## Multi-RDP Sessions (red/green/yellow/blue)

The app can hold up to four simultaneous RDP sessions to different servers, mapped to
the TV remote's color buttons in the remote's own order: red, green, yellow, blue
(slot ids 0..3):

- **Any color button** → "go to that session's screen": the live stream when the slot is
  connected, a direct connection attempt when its saved profile is offline, or that
  colour's setup drawer when it is empty. The app starts on the four-profile hub,
  selecting the first configured profile (red when none is configured). **Save and
  connect** always connects the profile whose drawer is open. Exiting is left to webOS
  itself (EXIT/home button -> system close, delivered to the app as
  SDL_QUIT/SDL_APP_TERMINATING).
- **One press of the remote's central OK button** while a desktop is visible opens HUB.
  An H.264 session remains on the fullscreen hardware plane; a RemoteFX session's cached
  RGBA desktop is drawn into the same SDL backbuffer as HUB before its translucent chrome.
  Both paths remain visible beneath one darkening mask: a left-to-right
  96/90/62/22/0% gradient and a bottom-to-top 92/80/50/18/0% gradient. Their clear
  upper-right area leaves the stream untouched; browsing cards never swaps the background.
  USB input is released, and selecting a connected
  card resumes or switches explicitly. Remote OK is identified by its evdev device origin,
  so Enter on a physical USB keyboard still goes to the GNOME host. BACK closes a nested
  setup/onboarding layer first; on the unobscured HUB it returns to the session that was
  on screen when HUB opened. A new connection started from HUB keeps that return target
  until the new worker is ACTIVE, so validation/start/network failures remain returnable.
- **HUB D-pad navigation**: LEFT/RIGHT move across all four computer cards, UP enters
  the selected card's action controls, and the remaining directions move between
  Connect/Resume, Edit, Help, and the selected card. OK activates the focused element.
  Colour keys and CH +/- remain optional direct shortcuts.
- **Brand and type** follow the `dc_v3` prototype: the four-colour cube rotates in HUB
  and onboarding. Pre-generated IBM Plex Sans fonts render headings, controls, and
  prose; IBM Plex Mono renders values, metadata, fields, badges, and scales; JetBrains
  Mono is reserved for the `gnomecast_` wordmark. The TV package does not parse or ship
  runtime TTF files.
- Switching to a connected slot moves the VIDEO plane and — KVM-style — the keyboard and
  mouse to it; a colored square in the top-right corner confirms the active slot for
  ~1.5s. Navigating to another slot's setup drawer leaves the previous session running
  backgrounded (graphics suppressed server-side, audio still in the mix).
- **Audio is mixed from ALL connected sessions** — a backgrounded session keeps playing
  its sound. Each connected HUB card carries a compact stereo VU indicator; its rails
  follow the post-fader L/R levels once the stream opens. The hero line stays neutral
  until the first stream arrives, then shows the negotiated codec, sample rate, and
  channel layout. The client
  advertises Opus 48k + PCM; gnome-remote-desktop prefers Opus
  (~96kbps per session instead of ~1.4Mbps raw), each session decodes it in-process via
  libopus on its own worker thread, and the headless miniaudio engine sums `ma_sound`
  voices as float PCM at 48kHz stereo in 480-frame (10ms) blocks. Each session has its
  own SPSC ring and dynamic converter, so 44.1kHz PCM and 48kHz Opus can play
  simultaneously. NDL receives paced
  S16LE from the engine pump. PCM-only servers keep working unchanged; builds with
  `HELLOLG_WITH_OPUS=OFF` mute Opus sessions with a log.
- **Audio quality** is a global setting (`audioCodec`, also a pre-connect UI dropdown and
  the `--audio-codec` CLI flag): `auto` (default) lets the server pick Opus; `pcm` offers
  the server PCM only, trading ~1.4Mbps per session for a lossless stream. It applies to
  connections made after the change. grd's PCM is 44.1kHz while Opus decodes at 48kHz;
  the per-source converters handle both concurrently without re-pinning the sink.
- **Adaptive delay is always enabled and has no user control.** Every source starts at
  60ms, ranges from 40–150ms, and derives its target from a rolling 10s histogram of
  relative arrival/capture-time variation (`p95 + 20ms`, 5ms buckets). RDPEA timestamp
  wrap is handled; zero/repeated timestamps fall back to decoded duration. A gap over
  500ms is a talkspurt boundary and re-primes at least 60ms rather than polluting the
  jitter estimate. New peaks and underruns raise the target immediately; stable delivery
  can lower it by at most 10ms per five seconds, never below 40ms.
- Queue error beyond 10ms changes each source's SRC ratio smoothly, capped at +/-0.5%
  by a 50ms error, with a slow drift term for mismatched producer/DAC clocks. A backlog
  over target by 80ms fades out for 5ms, drops old frames to `target + 20ms`, resets the
  converter, and fades in for 5ms. Underrun inserts silence and re-buffers only that
  source. The 1.5s ring is an emergency limit, not normal latency; producer overflow
  requests a consumer-side trim and never moves the consumer cursor.
- The mixer overlay shows `queue/target ms` above every session. Detailed p95 jitter,
  SRC ppm, underrun, hard-correction, and overflow counters are logged as one snapshot no
  more often than every three seconds.
- A backgrounded session's graphics are paused server-side via TS_SUPPRESS_OUTPUT_PDU.
  Because grd resumes with a delta frame and rejects Refresh Rect, switching back forces
  a fresh IDR by re-submitting the Display Control monitor layout (grd rebuilds its
  encode session on every layout PDU, even an identical one → RESET_GRAPHICS + IDR). If
  no decodable frame arrives within 2s of a switch (e.g. grd mirror mode has no Display
  Control channel), the client reconnects that session once — a fresh connection always
  starts with an IDR — and remembers the slot as refresh-ineffective, so later switches
  skip the doomed wait.
- **IDR-snapshot backgrounding** (refresh-ineffective slots): a plain suppress would
  strand such a slot behind an unusable delta chain (grd emits its only IDR at connect),
  so leaving its stream instead reconnects the slot invisibly — behind the new active
  screen — and caches the fresh connection's compressed IDR (+ any deltas racing the
  suppress) in an 8MB/slot AU snapshot; the server is suppressed once the IDR is in
  hand. Switching back replays the cache into the shared decoder (same-size in-band
  handover, no pipeline reload, no splash) and resumes output without a refresh: the
  server's next delta references exactly the replayed state. The first frame shown is
  the desktop as of backgrounding time; the resume delta catches it up. Cache overflow
  (a server that keeps streaming despite suppress) voids the snapshot and switch-back
  falls back to the visible reconnect. `HELLOLG_SNAPSHOT_FORCE=1` arms snapshot
  backgrounding for every slot without waiting for the watchdog to learn — the on-device
  experiment knob for validating that grd's resume delta really continues the cached
  chain.
- **Deferred switching**: a color-key press whose target stream is not ready (no cached
  IDR yet — first switch to a slot, a manual-Connect flag reset, a flip-back racing the
  suppress tail) no longer moves the screen into a black reload window. The CURRENT
  stream keeps playing (the badge shows the destination), the target fills its snapshot
  in the background — keyframe request first, hidden reconnect after the 2s no-keyframe
  deadline (which is also where refresh-ineffective is learned, now invisibly) — and the
  switch completes by replay once the cache is ready and quiet (~0.5s). Re-pressing the
  CURRENT slot's key cancels the deferred switch. Trade-off accepted deliberately: the
  hidden reconnect of a backgrounding slot interrupts that session's own audio in the
  mix for ~1-2s (`source N ran dry` + a one-cut backlog trim on rejoin) — the price of
  instant switch-backs on servers that only ever emit their connect IDR.
- **Channel-rocker ring switching**: during streaming CH_UP/CH_DOWN zap through the
  connected slots (red → green → yellow → blue → red; empty slots are skipped and stay
  reachable via their color buttons). Handled on both delivery paths like the color
  keys: SDL scancodes 480/481 from the ungrabbed RCU node and evdev KEY_CHANNELUP/DOWN
  402/403 from grabbed remote nodes. Channel keys mean nothing inside an RDP session,
  so nothing is taken from the remote desktop; the D-pad stays free for the mixer
  overlay. SDL key presses during streaming are logged (`remote sdl key scancode=`) —
  with the keyboard evdev-grabbed, only the remote and the system reach SDL, so the log
  maps what a given remote firmware actually sends.
- **Re-pressing the ACTIVE slot's color button** (while its stream is on screen) opens the
  volume mixer: a compact floating console above the desktop, with a rounded border and
  one fader channel per slot. Each channel is
  an L/R pair of LIVE volume-meter columns (post-fader peak, instant attack / 30 dB/s
  release, gradient anchored to the scale) with one white knob across both and a dBFS
  scale: -60 at the bottom stop (= full mute) up to an unmarked +6 dB headroom above the
  0 line. Up/down move the selected fader 3 dB per press (applied to the live mix
  immediately); left/right — or another slot's color button — change the selection; the
  active slot's button, OK, or Back closes it, and it auto-hides after ~6s without input.
  Mute, Duck, and Solo are shown as the single-letter console controls `M`, `D`, and `S`.
  While it is open the pointer belongs to the SYSTEM (the evdev grab is released and the
  plain arrow shown): click a fader to jump/drag it, click a channel elsewhere to select
  it, scroll the wheel for 3 dB steps, click outside the panel to close.
- **MASTER fader** (white, rightmost channel): mirrors and drives the webOS SYSTEM
  volume (`luna://com.webos.audio` get/setVolume — undocumented-but-working from the
  app's jail; officially LG only documents volumeUp/Down/setMuted, so an unavailable
  bus just leaves the channel dimmed). Its percentage guide labels share the same
  horizontal lines as the source dB scales. Those guide labels are rounded to the
  nearest 5%, while the large current value always shows the exact system percentage.
  It never touches the app's own mix: its meters
  show the mix OUTPUT (the summed, saturated chunk leaving for the audio track) and its
  knob travels volume percent over the full track while wearing the same dBFS scale
  artwork as the other channels (deliberate — a uniform bank; the user chose the "wrong"
  scale over a mismatched one). Steps of 3 per key press/wheel notch — one remote VOL
  press moves the system volume by 3, so keys and fader land on the same grid.
  TRANSPORT IS THE NATIVE luna-service2 CLIENT on a GMainLoop thread — NEVER a
  luna-send-pub subprocess: fork()ing the app while the NDL/OMX pipeline (re)loads
  races its in-process decoder state (black screens observed live), and a pipe-spawned
  subscriber cannot deliver events anyway (block-buffered stdout, no stdbuf on the TV).
  Volume-change SUBSCRIPTIONS DO NOT WORK for a dev-mode app on webOS TV Lite
  (webOS 23 generation, internal release 8.x) — probed live:
  com.webos.audio/getVolume accepts the subscribe but the DYNAMIC service idles out and
  takes it along ("com.webos.audio is not running"; zero events even while alive);
  master/getVolume subscribe → "Message status unknown"; apiadapter (the route external
  SSAP controllers get their change events from) → "Not permitted"; a NAMED bus client
  → "Invalid permissions" (the jail allows anonymous only). The cache is therefore kept
  live by polling getVolume every 200ms while streaming — in-process LS2 one-shots,
  cheap, and each call wakes the dozing service. Set calls are coalesced to the newest
  value on the loop thread; the cache updates optimistically for a snappy knob.
- **Auto-raise on volume change**: while streaming, any system-volume change made
  outside the overlay (remote VOL keys, BT-headphone buttons) pops the mixer with the
  MASTER channel selected; while the volume keeps moving the auto-hide timer keeps
  resetting, and once it settles the ~6s idle timeout hides the panel as usual. The
  first reading after launch is the baseline, so nothing pops at startup. No mouse or
  keyboard input reaches the RDP session while the panel is up; the grab and the
  session's cursor shape come back when it closes. Levels are per launch (not persisted), survive reconnects
  and source format resets, and disconnected slots show dimmed knobs whose level applies once
  they connect. On the RemoteFX RGBA fallback path the overlay cannot be drawn over the
  stream, so the button keeps its old badge-flash behavior there.
- If the ACTIVE session drops while another one is alive, video auto-switches to a
  survivor; if none is left, the pre-connect UI returns.

## Local Config

Local native config lives at `native/config.local.json`. It is gitignored and may contain
passwords. Do not commit it or paste it into logs.

Example shape (legacy flat form, applies to the green slot):

```json
{
  "host": "192.0.2.10",
  "port": 3389,
  "username": "gnome-user",
  "password": "replace-with-local-secret",
  "domain": "",
  "fps": 60,
  "wheelStep": 60,
  "wheelScrollDivisor": 1
}
```

All slots can be configured with the session-array shape (this is also what the app persists).
`name` is an optional display label; older files without it remain valid:

```json
{
  "sessions": [
    { "slot": "green", "name": "Studio PC", "host": "192.0.2.10", "port": 3389, "username": "u", "password": "...", "domain": "", "fps": 60 },
    { "slot": "yellow", "name": "Media Server", "host": "192.0.2.11", "port": 3389, "username": "u", "password": "...", "domain": "", "fps": 60 }
  ],
  "wheelStep": 60,
  "wheelScrollDivisor": 1,
  "audioCodec": "auto"
}
```

The hub's setup drawer saves a profile immediately on **Save** or **Save and connect**, so
a failed first connection does not discard the optional profile name, address, username,
domain, password, or FPS. **Delete profile** removes that colour's saved credentials after
confirmation. Unsaved drafts in other drawers are never included. The save path is
resolved from a candidate list (each rejection is logged with its reason); on the TV the
winner is the in-app
`<approot>/settings/<euid>/settings.json` — the IPK ships `settings/` mode 01777 because
the install tree is root-owned, and the app creates a private 0700 subdir inside (same
trust model as /tmp). Verified on device: this survives app restarts, package
reinstalls AND TV power cycles; `/media/developer/temp/<appid>-<euid>` is the next
candidate (persistent ext4), and the tmpfs `/tmp/<appid>-<euid>` fallback is last —
loading scans the same priority order, so settings migrate upward on the next save.
Explicit webOS launch params or CLI flags override loaded settings (flat launch/CLI keys
target the green slot). Set `HELLOLG_IGNORE_SAVED_CONFIG=1` for a one-off launch that
ignores saved UI settings.

Changing connection fields on a still-live background profile and pressing **Save** ends
that old worker, so the newly labelled profile can never resume the previous computer by
mistake. **Save and connect** replaces it immediately; a name-only edit leaves it running.

`audioPrebufferMs` and `--audio-prebuffer-ms` are accepted for one compatibility release,
ignored, and produce one deprecation warning. Persisted JSON no longer writes the field;
old value `0` does not disable buffering.

## Local Build And Test

Targeted local loop:

```sh
./tools/syntax-check-native.sh
cargo test --manifest-path webrdp-min/Cargo.toml --features native native::tests::
cmake -S native -B /tmp/gnomecast-native-build-tests
cmake --build /tmp/gnomecast-native-build-tests
ctest --test-dir /tmp/gnomecast-native-build-tests --output-on-failure
```

Add `-DHELLOLG_WITH_OPUS=ON` to the configure step to include the `audio-opus` test; the
flag is off by default on host builds (ExternalOPUS downloads the libopus source tarball)
and on only for webOS cross-builds.

Build and link the native shell against the real Rust static library:

```sh
cargo build --manifest-path webrdp-min/Cargo.toml --features native
cmake -S native -B /tmp/gnomecast-native-rust-build \
  -DHELLOLG_LINK_RDP_FFI=ON \
  -DRDP_FFI_LIB="$PWD/webrdp-min/target/debug/libwebrdp_min.a"
cmake --build /tmp/gnomecast-native-rust-build
```

## webOS Build, Package, Install, Launch

The product webOS build requires:

- webOS buildroot toolchain;
- initialized `third_party/IronRDP`,
  `third_party/lvgl`, and `third_party/miniaudio` submodules;
- SDL2 for the webOS target;
- Rust target support for `armv7-unknown-linux-gnueabi`.

Default toolchain path:

```text
/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
```

Build and package:

```sh
./tools/build-native-webos.sh
```

Do not use `arm-unknown-linux-gnueabi` for the Rust staticlib on TV. It can emit
legacy CP15 barrier instructions that crash with `Illegal instruction` on ARMv8 webOS.

`tools/build-native-webos.sh` builds the Rust armv7 staticlib in `release` mode by
default, configures the product CMake build with NDL/SDL/LVGL/RDP FFI enabled,
stages the app, verifies the staged tree and IPK, and writes the package under
`dist/native-webos/`. Use `NATIVE_WEBOS_RUST_PROFILE=debug` only for diagnostics.

Install and launch:

```sh
ARES_DEVICE=<tv-device> HELLOLG_NATIVE_CONFIG=native/config.local.json \
  ./tools/deploy-native-webos.sh
```

`tools/deploy-native-webos.sh` installs the latest IPK, reads the host-side config file,
and sends supported fields
with `ares-launch --params` without printing the generated JSON. The native app opens a
four-profile session hub; each colour-key card has a right-side setup drawer for its name,
address, username, domain, password, `30/60 FPS`, and global audio-quality preference.
The local SDL graphics/UI surface is
fixed at 1920x1080 (webOS always scales this virtual canvas to the panel; the video decoder
plane runs at the server's real resolution independently, so a larger local surface has no
benefit — see the EGFX surface-size note below). The RDP initial desktop request is a fixed
hint, and the runtime desktop size reported by the server remains the source of truth for
stream/input sizing. Use `--with-defaults` only for an explicit defaults-only startup smoke.

The native app id is `com.truebest.gnomecast.native`. The native package must contain the
native executable and native `appinfo.json`; package verification rejects browser/runtime files such as
`*.html`, `*.js`, `package.json`, and historical `app/` or `service/` trees.

## Audio Device Acceptance

NDL is the selected production sink. Accept it on TV with
30 minutes of 4K/60 video plus continuous audio, at least two audible sessions, stable
target returning to no more than 60ms, no underrun for jitter within the current target,
and recovery from gaps above 150ms without permanent backlog. CPU regression must stay
within 5 percentage points and RSS within 4MB.

An independent SDL callback sink was considered but is not part of the rollout: retaining
NDL avoids qualifying a second webOS device path and keeps audio behavior consistent with
the already proven TV media stack. Do not add an SDL audio sink or a custom NDL `ma_device`
backend without a new explicit design decision. WSOLA/PLC remains deferred unless the
current +/-0.5% SRC and rare fade/drop corrections prove insufficient.

## Native logging

The C shell, Rust RDP core, and the NDL adapter all end at the embedded `clog` C11
logger. A C source file defines its default prefix/category once, then ordinary calls
contain only the level and message:

```c
clog_define(g_native_log_config, cLogLevelTrace, cLogFlags_Default, "ExampleApp", NULL);

clog(cLogLevelNotice, "Hello, World!\n");
clog(cLogLevelError, "Error: %d\n", err);
```

`clog_define` is file-scoped and appears exactly once per production application translation
unit. The exceptions are `category_log.c` (the logger engine itself) and generated font
sources. The standalone `backend_ndl` library retains its callback API instead of depending
on the application logger. A category string is both the printed prefix and the selector
used for live configuration. It is 1-63 ASCII bytes: alphanumeric, `_`, or `-` components
separated by single dots.
Default flags print local wall time, monotonic time since launch, severity, prefix, and
automatic source metadata for trace/debug messages. Individual flags can select those
fields, disable all metadata with `cLogFlags_None`, or force source metadata. The final config
argument is reserved and must currently be `NULL`. The lowercase `clog` macro intentionally
owns the same name as the C99 complex logarithm, so a translation unit using `<complex.h>`
must not include `clog.h`.

By default the application prints `info` and higher to stderr; the native executable
redirects stderr to `/tmp/gnomecast-native.log` unless `HELLOLG_NATIVE_LOG_PATH` overrides
the path. Set `GNOMECAST_LOG` before launch to change thresholds:

```sh
GNOMECAST_LOG='*=info,native=debug,rdp=debug,audio=debug,media.ndl=debug,input=warn'
```

This is an environment variable of the native process. The host-side
`deploy-native-webos.sh` environment is not forwarded by `ares-launch`; use it for direct
launches/debug shells or call the live `clog_configure()` API from an in-process debug
control surface.

Rules are comma-separated, case-sensitive `selector=level` pairs. A selector matches both
the named prefix and dot-separated descendants; later rules win, and an empty value
restores compiled defaults. Levels are `trace`, `debug`, `info`, `notice`, `warn`, `error`,
`fatal`, and `off`. Invalid rule strings are rejected as a unit. Application prefixes are
`native`, `config.paths`, `config.settings`, `video.snapshot`, `video.ndl`, `audio.opus`,
`audio.pipeline`, `audio.ndl`, `input.evdev`, `input.sdl`, `media.ndl`, `ui.preconnect`,
`ui.mixer`, `cursor`, `luna.volume`, `video.h264`, `video.rgba`, `rdp.rust`, and `rdp.stub`.
The `media.ndl` category also carries the standalone backend_ndl library log, forwarded
through the media adapter's callback. Rust/IronRDP events use `rdp.rust`; their original
tracing target (including `webrdp.transport`, `webrdp.session`, `webrdp.graphics`, and
`webrdp.audio`) is retained in the message. Parent selectors such as `audio=debug` or
`rdp=debug` cover every dotted child. The logger deliberately does not print credentials
or config-file contents.

`GNOMECAST_LOG` is the only runtime log-level control. The obsolete `WEBRDP_LOG` and
`GNOMECAST_NDL_LOG` variables and the `/tmp/gnomecast-ndl-debug` marker are not supported.
Rust probes the enabled `rdp.rust` levels when a session worker starts (pruning disabled
trace/debug callsites), re-checks the level before formatting each surviving event, and
forwards the structured level, tracing target, and message synchronously. Events outside
an active RDP worker have no native callback and are dropped.

`tools/check-logging-policy.sh`, invoked by `tools/syntax-check-native.sh`, enforces category
coverage and rejects direct stderr/SDL logging in application C plus stderr logging in
production Rust.

Diagnostic assertions are compiled out of product (`NDEBUG`) builds. In debug builds an
assert can include a backtrace when the target libc supports it; debugger breaks remain
off unless the `diagnostics.break` category is explicitly enabled and a debugger is
actually attached.

## NDL Backend Smoke (on-TV, run after backend changes)

Watch `/tmp/gnomecast-native.log` over ssh while exercising. For sink-level
telemetry (audio buffer available/total every ~5s, video render queue depth,
per-drop lines), launch the native process with
`GNOMECAST_LOG='media.ndl=debug'`. The adapter mirrors the effective `media.ndl` level
into the backend's minimum log level when media opens, so debug telemetry is not even
produced while the level is filtered out. Audio drop episodes always end with an INFO
`audio sink recovered: dropped N block(s)` summary, so the default log distinguishes a
transient drop from a dead sink.

1. **Probe/init**: startup shows `[backend-ndl] loaded NDL library: libNDL_directmedia.so.1`
   (or which candidate/RTLD_DEFAULT won), any missing optional symbols, and
   `initialized app_id=com.truebest.gnomecast.native`. A dlopen or `NDL_DirectMediaInit`
   failure logs the exact `NDL_DirectMediaGetError()` text.
2. **Connect**: green session → video within ~2 s of the first IDR
   (`media loaded: generation=N video=yes`), audio starts immediately (the empty-frame
   priming runs after every load; a long first-audio delay = priming regression).
3. **Server resolution change**: desktop resize → video track reopen on the next IDR,
   audio recovers after the reload (`reopening video on next keyframe` flow in main.c).
4. **Session switching (color buttons)**: snapshot replay succeeds — replayed AUs must
   count as progress (`fed N AUs` advances); all-DROPPED replay means the pipeline was
   fed while unloaded.
5. **Audio-under-live-video open**: first audio negotiation after video is up re-arms
   the keyframe gate (`NEED_KEYFRAME` → refresh request → picture recovers).
6. **Background/relaunch**: Home out and relaunch; media tears down only on real exit.
7. **30 min soak**: RSS stable (`/proc/<pid>/status` over ssh), no log spam from
   `not ready`/`overflow` (each is log-once per episode), no A/V drift.

`resource released by firmware:` in the log means the TV reclaimed the decoder (another
app took it) — expect a frozen plane until the next reload; recovery policy is a known
open item.

## Triage

- Config failures: verify `native/config.local.json` exists and is readable; do not print it.
- TLS/CredSSP failures before `RDP_STATE_ACTIVE` should surface as `NetworkError`,
  `ProtocolError`, or a specific TLS/CredSSP diagnostic.
- H.264-capable sessions should use the NDL hardware path; if the server cannot provide H.264,
  verify native RemoteFX bitmap updates reach the SDL RGBA presenter.
- The server's real graphics output size (from RDPGFX_RESET_GRAPHICS_PDU) can differ from the
  negotiated MCS/GCC desktop size. This is expected on webOS: the app's graphics/UI plane
  always renders on a virtual ~1920x1080 (or 1280x720 on HD-only models) logical canvas that
  the platform scales to the panel, while the separate hardware video decoder plane can output
  at the panel's true native resolution (up to 4K/8K) independently of the UI plane and of the
  negotiated RDP session size. `on_desktop_size` is re-invoked on every
  RDPGFX_RESET_GRAPHICS_PDU (not just the initial MCS/GCC handshake), so `App.desktop_width`/
  `desktop_height` and the pointer input mapping stay in sync with the real EGFX size for both
  the NDL/H.264 and RemoteFX RGBA paths; `on_video_au` reopens the video track whenever that
  size no longer matches what is currently open. This applies to any webOS target with a
  higher-than-FHD panel, not just one device.
- The SDL graphics layer presents one transparent frame to punch through to the NDL hardware
  video plane, then stops touching the window (`App.video_plane_punched`). Re-presenting every
  loop tick raced the video plane's own buffer swaps and produced visible flicker.
- The server's cursor shapes (RDP pointer updates) are rendered through the platform cursor
  plane (`SDL_CreateColorCursor`, see `cursor_sdl.c`) — never as an SDL overlay, which would
  need per-tick presents and re-introduce the flicker above. Check the log for
  `DEBUG cursor: server cursor WxH ...` on connect (enable it with
  `GNOMECAST_LOG='cursor=debug'`); `WARN cursor: color cursor
  unavailable: ...` means the webOS SDL port refused color cursors and the client stays on
  the default arrow. The mouse is read from grabbed `/dev/input` (evdev) and the cursor is
  driven by warping the OS pointer to the logical position, so server shapes ride the real
  pointer; visibility follows the server's pointer state alone.
- Video track loads (`NDL_DirectMediaLoad(video=1 ...)`) but `LoadCallback
  STATE_UPDATE_LOADCOMPLETED/PLAYING` never follow while `DEBUG video.ndl: fed N AUs` keeps
  growing → the server is streaming at a resolution the TV's hardware pipeline cannot
  start on. Observed live with a 2048x1152 virtual display (black screen, zero errors);
  standard video resolutions (1920x1080, 3840x2160) are confirmed good — keep the server
  display on one of those. The Display Control DVC prevents this by forcing the
  configured resolution at connect — check the log for `display: requesting server
  resolution ...`; its absence means the server never opened the MS-RDPEDISP channel
  (older gnome-remote-desktop versions do not), in which case the resolution must be
  fixed server-side.
- Decoder/render failures should surface as `DecoderError` and must not fall back to
  MSE, WebCodecs, RDCleanPath, or browser rendering.
- Input issues: run `ctest --test-dir /tmp/gnomecast-native-build-tests -R input-sdl --output-on-failure`.

## Current Status

Implemented:

- Native CMake target and native `appinfo.json` for app id `com.truebest.gnomecast.native`.
- Frozen C ABI in `native/include/rdp_ffi.h`.
- Rust `native` feature exporting FFI lifecycle, input symbols, direct TCP/TLS/CredSSP
  worker scaffold, AVC420 callbacks, and native RemoteFX/bitmap callbacks.
- C FFI stub for local scaffold builds; product webOS builds require the Rust staticlib.
- Pinned native dependency submodules, including miniaudio 0.11.25, with provenance and
  license staging.
- Standalone `backend_ndl/` DirectMedia library plus gnomecast adapters in
  `native/src/ndl_adapter/`: dlopen, atomic combined-track loads, callbacks, optional PCM
  priming/keyframe gating, and application-owned H.264 framing/recovery policy.
- H.264 framing normalization, including AU size caps. NOTE: the wire carries BOTH
  framings — the nominal RDPEGFX shape is AVC length-prefixed (converted to Annex-B),
  but gnome-remote-desktop delivers Annex-B outright (passed through). Any code
  classifying AUs must handle both — use the `h264_annexb.h` scanners, never a
  hand-rolled parser (a length-prefix-only IDR check once silently killed snapshot
  switching in the field).
- Native RGBA surface helper for RemoteFX/bitmap dirty rectangles.
- CTests for ABI layout, H.264 scanning/conversion/keyframe detection, and input helpers.
- SDL/webOS fullscreen event loop and input dispatch in product builds.
- Two native webOS helper scripts: build/package/verify and deploy/launch.
- Native package/install/launch verification on the TV.
- Live fullscreen GNOME desktop through the NDL hardware plane, TV-verified (AVC420),
  including correct behavior when the server's real EGFX surface
  resolution differs from the negotiated MCS/GCC desktop size.
- Multi-RDP: four session slots (red/green/yellow/blue, remote-button order)
  with color-button screen navigation (stream when connected, direct connect when a
  saved profile is offline, setup drawer when empty),
  KVM-style input switching, mixed PCM audio from all connected sessions,
  suppress-output backgrounding, Display-Control-based keyframe refresh with a
  reconnect watchdog, session-array persisted settings with legacy fallback.
- Headless miniaudio float engine with per-session `ma_sound` voices, independent source
  rates, adaptive jitter/drift control, gain/meters, and an S16 NDL pump.

Not yet implemented or not yet TV-verified:

- Native RemoteFX/bitmap fallback path against a server without H.264.
- Live fullscreen RemoteFX-only/native RGBA presentation on TV.
- Multi-RDP on-device verification: color-button scancodes during streaming, switch
  latency, simultaneous audio mix, RSS with two sessions, background session surviving a
  switch (Phase 0 device checks from the multi-RDP plan).
- Miniaudio/NDL device acceptance described above.

## Third-Party Provenance

Initialize pinned native dependencies:

```sh
git submodule update --init third_party/IronRDP third_party/lvgl third_party/miniaudio
```

See `third_party/PROVENANCE.md` for pinned commits, licenses, and the Moonlight reference boundary.
