# Native ss4s Runbook

This runbook tracks the current native-only webOS target. The native app must not fall
back to a web app, JavaScript service, MSE, WebCodecs, RDCleanPath, or browser rendering.
AVC420/H.264 is preferred through ss4s/NDL/SMP; RemoteFX is supported only as a native
Rust/IronRDP decode path with RGBA updates presented by SDL.

## Multi-RDP Sessions (red/green/yellow/blue)

The app can hold up to four simultaneous RDP sessions to different servers, mapped to
the TV remote's color buttons in the remote's own order: red, green, yellow, blue
(slot ids 0..3):

- **Any color button** → "go to that session's screen": the live stream when the slot is
  connected, its session configurator (pre-connect form) otherwise. The app starts on
  the green configurator. Connect always connects the slot whose configurator is on
  screen. Exiting is left to webOS itself (EXIT/home button -> system close, delivered
  to the app as SDL_QUIT/SDL_APP_TERMINATING).
- Switching to a connected slot moves the VIDEO plane and — KVM-style — the keyboard and
  mouse to it; a colored square in the top-right corner confirms the active slot for
  ~1.5s. Navigating to a configurator leaves the previous session running backgrounded
  (graphics suppressed server-side, audio still in the mix).
- **Audio is mixed from ALL connected sessions** — a backgrounded session keeps playing
  its sound. The client advertises Opus 48k + PCM; gnome-remote-desktop prefers Opus
  (~96kbps per session instead of ~1.4Mbps raw), each session decodes it in-process via
  libopus on its own worker thread, and the native mixer sums the PCM (per-source jitter
  prebuffer = `audioPrebufferMs`, saturating sum, ring capacity = the latency cap,
  ~1.4s). PCM-only servers keep working unchanged; builds with `HELLOLG_WITH_OPUS=OFF`
  mute Opus sessions with a log.
- **Audio quality** is a global setting (`audioCodec`, also a pre-connect UI dropdown and
  the `--audio-codec` CLI flag): `auto` (default) lets the server pick Opus; `pcm` offers
  the server PCM only, trading ~1.4Mbps per session for a lossless stream. It applies to
  connections made after the change. Note grd's PCM is 44.1kHz while Opus decodes at
  48kHz — with no resampler the mix runs at one rate, so sessions negotiated under the
  OTHER preference stay muted until they reconnect.
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
  volume mixer: a semi-transparent panel with one fader channel per slot. Each channel is
  an L/R pair of LIVE volume-meter columns (post-fader peak, instant attack / 30 dB/s
  release, gradient anchored to the scale) with one white knob across both and a dBFS
  scale: -60 at the bottom stop (= full mute) up to an unmarked +6 dB headroom above the
  0 line. Up/down move the selected fader 3 dB per press (applied to the live mix
  immediately); left/right — or another slot's color button — change the selection; the
  active slot's button, OK, or Back closes it, and it auto-hides after ~6s without input.
  While it is open the pointer belongs to the SYSTEM (the evdev grab is released and the
  plain arrow shown): click a fader to jump/drag it, click a channel elsewhere to select
  it, scroll the wheel for 3 dB steps, click outside the panel to close.
- **MASTER fader** (white, rightmost channel): mirrors and drives the webOS SYSTEM
  volume (`luna://com.webos.audio` get/setVolume — undocumented-but-working from the
  app's jail; officially LG only documents volumeUp/Down/setMuted, so an unavailable
  bus just leaves the channel dimmed). It never touches the app's own mix: its meters
  show the mix OUTPUT (the summed, saturated chunk leaving for the audio track) and its
  knob travels volume percent over the full track while wearing the same dBFS scale
  artwork as the other channels (deliberate — a uniform bank; the user chose the "wrong"
  scale over a mismatched one). Steps of 3 per key press/wheel notch — one remote VOL
  press moves the system volume by 3, so keys and fader land on the same grid.
  TRANSPORT IS THE NATIVE luna-service2 CLIENT on a GMainLoop thread — NEVER a
  luna-send-pub subprocess: fork()ing the app while the NDL/OMX pipeline (re)loads
  races its in-process decoder state (black screens observed live), and a pipe-spawned
  subscriber cannot deliver events anyway (block-buffered stdout, no stdbuf on the TV).
  Volume-change SUBSCRIPTIONS DO NOT WORK for a dev-mode app on webOS 24 — probed live:
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
  and codec re-pins, and disconnected slots show dimmed knobs whose level applies once
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

Both slots can be configured with the v2 shape (this is also what the app persists):

```json
{
  "sessions": [
    { "slot": "green", "host": "192.0.2.10", "port": 3389, "username": "u", "password": "...", "domain": "", "fps": 60 },
    { "slot": "yellow", "host": "192.0.2.11", "port": 3389, "username": "u", "password": "...", "domain": "", "fps": 60 }
  ],
  "wheelStep": 60,
  "wheelScrollDivisor": 1,
  "audioPrebufferMs": 60,
  "audioCodec": "auto"
}
```

The pre-connect UI saves the settings of ALL slots on a successful Connect, including
address, username, domain, password, and FPS. The save path is resolved from a candidate
list (each rejection is logged with its reason); on the TV the winner is the in-app
`<approot>/settings/<euid>/settings.json` — the IPK ships `settings/` mode 01777 because
the install tree is root-owned, and the app creates a private 0700 subdir inside (same
trust model as /tmp). Verified on device: this survives app restarts, package
reinstalls AND TV power cycles; `/media/developer/temp/<appid>-<euid>` is the next
candidate (persistent ext4), and the tmpfs `/tmp/<appid>-<euid>` fallback is last —
loading scans the same priority order, so settings migrate upward on the next save.
Explicit webOS launch params or CLI flags override loaded settings (flat launch/CLI keys
target the green slot). Set `HELLOLG_IGNORE_SAVED_CONFIG=1` for a one-off launch that
ignores saved UI settings.

## Local Build And Test

Targeted local loop:

```sh
cc -fsyntax-only -Inative/include \
  native/src/main.c native/src/media_ss4s.c native/src/video_ss4s.c \
  native/src/audio_ss4s.c native/src/audio_mixer.c native/src/audio_opus.c \
  native/src/video_rgba_sdl.c native/src/au_snapshot.c \
  native/src/h264_annexb.c native/src/input_sdl.c native/src/cursor_sdl.c \
  native/src/rdp_ffi_stub.c native/src/config_paths.c native/src/settings_json.c
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
- initialized `third_party/IronRDP`, `third_party/ss4s`, `third_party/commons`, and `third_party/lvgl` submodules;
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
default, configures the product CMake build with ss4s/SDL/LVGL/RDP FFI enabled,
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
pre-connect UI where address, username, domain, and password can be edited before
pressing Connect. The UI exposes a `30/60 FPS` selector. The local SDL graphics/UI surface is
fixed at 1920x1080 (webOS always scales this virtual canvas to the panel; the video decoder
plane runs at the server's real resolution independently, so a larger local surface has no
benefit — see the EGFX surface-size note below). The RDP initial desktop request is a fixed
hint, and the runtime desktop size reported by the server remains the source of truth for
stream/input sizing. Use `--with-defaults` only for an explicit defaults-only startup smoke.

The native app id is `com.truebest.gnomecast.native`. The native package must contain the
native executable and native `appinfo.json`; package verification rejects browser/runtime files such as
`*.html`, `*.js`, `package.json`, and historical `app/` or `service/` trees.

## Triage

- Config failures: verify `native/config.local.json` exists and is readable; do not print it.
- TLS/CredSSP failures before `RDP_STATE_ACTIVE` should surface as `NetworkError`,
  `ProtocolError`, or a specific TLS/CredSSP diagnostic.
- H.264-capable sessions should use the ss4s hardware path; if the server cannot provide H.264,
  verify native RemoteFX bitmap updates reach the SDL RGBA presenter.
- The server's real graphics output size (from RDPGFX_RESET_GRAPHICS_PDU) can differ from the
  negotiated MCS/GCC desktop size. This is expected on webOS: the app's graphics/UI plane
  always renders on a virtual ~1920x1080 (or 1280x720 on HD-only models) logical canvas that
  the platform scales to the panel, while the separate hardware video decoder plane can output
  at the panel's true native resolution (up to 4K/8K) independently of the UI plane and of the
  negotiated RDP session size. `on_desktop_size` is re-invoked on every
  RDPGFX_RESET_GRAPHICS_PDU (not just the initial MCS/GCC handshake), so `App.desktop_width`/
  `desktop_height` and the pointer input mapping stay in sync with the real EGFX size for both
  the ss4s/H.264 and RemoteFX RGBA paths; `on_video_au` reopens the ss4s decoder whenever that
  size no longer matches what is currently open. This applies to any webOS target with a
  higher-than-FHD panel, not just one device.
- The SDL graphics layer presents one transparent frame to punch through to the ss4s hardware
  video plane, then stops touching the window (`App.video_plane_punched`). Re-presenting every
  loop tick raced the video plane's own buffer swaps and produced visible flicker.
- The server's cursor shapes (RDP pointer updates) are rendered through the platform cursor
  plane (`SDL_CreateColorCursor`, see `cursor_sdl.c`) — never as an SDL overlay, which would
  need per-tick presents and re-introduce the flicker above. Check the log for
  `[native-cursor] server cursor WxH ...` on connect; `[native-cursor] color cursor
  unavailable: ...` means the webOS SDL port refused color cursors and the client stays on
  the default arrow. The mouse is read from grabbed `/dev/input` (evdev) and the cursor is
  driven by warping the OS pointer to the logical position, so server shapes ride the real
  pointer; visibility follows the server's pointer state alone.
- Video track loads (`NDL_DirectMediaLoad(video=1 ...)`) but `LoadCallback
  STATE_UPDATE_LOADCOMPLETED/PLAYING` never follow while `[native-video] fed N AUs` keeps
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
- Pinned `third_party/ss4s` and `third_party/commons` submodules with provenance and license staging.
- ss4s NDL/SMP hardware-module selection, dummy backend rejection, and H.264 feed boundary.
- H.264 AVC length-prefixed to Annex-B normalization, including AU size caps.
- Native RGBA surface helper for RemoteFX/bitmap dirty rectangles.
- CTests for ABI layout, H.264 scanning/conversion/keyframe detection, and input helpers.
- SDL/webOS fullscreen event loop and input dispatch in product builds.
- Two native webOS helper scripts: build/package/verify and deploy/launch.
- Native package/install/launch verification on the TV.
- Live fullscreen GNOME desktop through the ss4s/NDL/SMP hardware plane, TV-verified
  (`ndl-webos5`/AVC420), including correct behavior when the server's real EGFX surface
  resolution differs from the negotiated MCS/GCC desktop size.
- Multi-RDP: four session slots (red/green/yellow/blue, remote-button order)
  with color-button screen navigation (stream when connected, configurator when not),
  KVM-style input switching, mixed PCM audio from all connected sessions,
  suppress-output backgrounding, Display-Control-based keyframe refresh with a
  reconnect watchdog, v2 persisted settings with legacy fallback.

Not yet implemented or not yet TV-verified:

- Native RemoteFX/bitmap fallback path against a server without H.264.
- Live fullscreen RemoteFX-only/native RGBA presentation on TV.
- Multi-RDP on-device verification: color-button scancodes during streaming, switch
  latency, simultaneous audio mix, RSS with two sessions, background session surviving a
  switch (Phase 0 device checks from the multi-RDP plan).

## Third-Party Provenance

Initialize pinned native dependencies:

```sh
git submodule update --init third_party/IronRDP third_party/ss4s third_party/commons third_party/lvgl
```

See `third_party/PROVENANCE.md` for pinned commits, licenses, and the Moonlight reference boundary.
