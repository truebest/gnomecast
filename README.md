# gnomecast

`gnomecast` is a native webOS RDP client for LG TVs, built to put GNOME desktops
(`gnome-remote-desktop`) on the TV with hardware-decoded video and native mixed audio.

## Features

- **Video**: AVC420/H.264 over RDPEGFX, decoded by the TV hardware video plane through
  the in-house NDL DirectMedia backend (`backend_ndl`, dlopen of
  `libNDL_directmedia.so.1`). Servers that cannot provide H.264 fall back to native
  RemoteFX Progressive/bitmap decoding in Rust, presented as RGBA through SDL.
- **Multi-session switching**: four RDP sessions map to the remote's red, green, yellow,
  and blue buttons. One session owns the video and KVM-style input at a time while the
  other connected sessions remain backgrounded and keep feeding the audio mix.
- **Audio**: MS-RDPEA over the `AUDIO_PLAYBACK_DVC` dynamic channel (the transport
  gnome-remote-desktop uses). Opus 48 kHz stereo is negotiated preferentially and
  decoded in-process through libopus; 16-bit PCM is the fallback. A headless miniaudio
  engine mixes `ma_sound` voices from all sessions at 48 kHz stereo and independently
  resamples 44.1/48 kHz sources. Its always-on adaptive jitter controller tracks
  burst/HOL delay and clock drift; there is no manual buffer setting. NDL remains the
  PCM sink. Audio is strictly best-effort: failures degrade to silent video, never
  to a dropped session.
- **Input**: the USB mouse and keyboard are read straight from the kernel evdev layer
  (`/dev/input`, `EVIOCGRAB`), below the webOS compositor, so physical input reaches the RDP
  server untouched by the TV's SDL/Wayland munging — no synthesized Back on right-click, no
  pointer recenter, no F-key/keypad/NumLock quirks, no IME double-input. Mouse motion, buttons
  and wheel and every key are forwarded as RDP fast-path input, keys as raw AT set-1 scancodes;
  NumLock is synced on connect so the numpad types digits. When no USB mouse is attached the
  compositor pointer (Magic Remote) still drives the session through an SDL fallback. The grab
  is global, so it follows window focus: a webOS overlay (the TV menu) that steals focus
  releases the mouse and keyboard so they drive the overlay, and re-grabs — restoring the
  cursor — on return; held buttons/keys are released to the server first so a mid-press focus
  change never leaves a stuck drag.
- **Server cursor**: the real pointer shapes (I-beam, resize arrows, ...) arrive as RDP
  pointer updates and are applied as the system color cursor, composited by the platform's
  cursor plane above the hardware video with zero extra presents. The grabbed mouse drives it
  by warping the OS pointer to the logical position; visibility follows the server's pointer
  state.
- **Pre-connect UI**: an on-TV LVGL settings screen (host, credentials, fps, audio codec)
  with persisted settings.
- **Network autodetection**: the client answers connect-time and continuous RTT
  measurements over the MCS message channel — gnome-remote-desktop refuses audio
  redirection without it.
- **Display Control (MS-RDPEDISP)**: right after connect the client tells the server to
  switch its (virtual) monitor to the configured resolution, so headless hosts with odd
  display defaults (e.g. 2048×1152) stream at a resolution the TV pipeline can decode
  (see Known limitations for servers without this channel).
- **Auto-reconnect**: gnome-remote-desktop closes sessions with a provider-initiated
  disconnect as a normal part of daemon handoffs; the client reconnects automatically
  (up to 3 attempts), matching mstsc/FreeRDP behavior.

## Remote host

The server side is gnome-remote-desktop with RDP enabled. For the hardware video path the
current known-good configuration is a host with an **NVIDIA GPU**: gnome-remote-desktop
then encodes with the GPU's hardware encoder (NVENC) and produces a clean AVC420
(H.264 4:2:0) stream that the TV decodes flawlessly on its hardware video plane.
gnome-remote-desktop also has VA-API and software encoding paths, but they have not been
tested with this client; when no usable H.264 encoder is available the server falls back
to RemoteFX Progressive, which this client renders in software at a noticeably higher
cost on both ends.

## Known limitations

- **Non-standard server resolutions do not play.** The TV's hardware video pipeline
  silently never starts on odd sizes — observed live with a 2048×1152 virtual display:
  black screen, no errors, frames are fed but playback never begins. Standard resolutions
  are confirmed good (1920×1080, 3840×2160). On servers that support MS-RDPEDISP the
  client fixes this automatically at connect; older gnome-remote-desktop versions never
  open that channel, so their (virtual) display must be set to a standard resolution
  server-side — and note that a headless server's display can revert to its odd default
  after a service restart.
- **AVC444 is not negotiated** — the client advertises AVC420 (4:2:0) only (see
  "Chroma subsampling" below). Servers without a usable H.264 encoder fall back to
  software RemoteFX rendering (slower, and rendered on the ~1080p UI plane rather than
  the native-resolution video plane).
- **EGFX surface-composition operations are ignored** (SolidFill, SurfaceToSurface,
  CacheToSurface). Harmless with gnome-remote-desktop — it sends SurfaceToSurface during
  routine AVC420 sessions and the picture is complete since the video plane carries full
  frames — but a server that relied on them for the software RemoteFX path would show
  stale regions.

### Chroma subsampling: why there is no AVC444 mode

The client negotiates AVC420 (H.264 4:2:0) only. This is a platform ceiling,
not a missing feature:

- RDP's AVC444 is not a single 4:4:4 stream. Per MS-RDPEGFX it is *two*
  AVC420 bitstreams (a luma frame plus an auxiliary frame carrying the packed
  chroma samples) that the client must decode independently and recombine in
  the pixel domain. webOS media pipelines (NDL/SMP) feed the elementary
  stream straight to the hardware video plane and never expose decoded frames
  to the application, so the recombination step has nowhere to run.
- On the validated webOS 23/24 targets, the available hardware profiles are H.264
  BP/MP/HP, HEVC Main/Main10, and AV1 Main — no Hi444PP, HEVC RExt, or AV1 High.
- Decoding the AVC444 stream pair in software would forfeit the hardware
  video plane and cannot sustain 4K on TV SoCs.

The same composition ceiling applies to any hardware-plane client that cannot access
decoded frames.

## Layout

- `native/` — C11/CMake shell: webOS lifecycle, raw evdev mouse+keyboard reader
  (`input_evdev.c`, with an SDL pointer fallback), SDL presentation, gnomecast-specific
  DirectMedia adapters under `native/src/ndl_adapter/`, RemoteFX RGBA presentation,
  pre-connect UI, and package targets.
- `backend_ndl/` — standalone MIT C11 DirectMedia library with a public SDK-independent
  header, CMake package, runtime `dlopen`, callbacks, and host tests.
- `webrdp-min/` — Rust static library implementing the RDP client (direct TCP + TLS +
  CredSSP/NTLM, EGFX, rdpsnd-over-DVC), exposed through the C ABI in
  `native/include/rdp_ffi.h`.
- `third_party/` — pinned native dependencies (git submodules): our IronRDP fork
  ([truebest/IronRDP](https://github.com/truebest/IronRDP), branch `gnome-rdp-support`,
  mirror patch record in `patches/ironrdp/`), LVGL, miniaudio.
- `Dockerfile` + `bitbucket-pipelines.yml` — reproducible build environment and CI that
  produces the webOS `.ipk`. CI runs inside a prebuilt public image
  (`cubicattache/gnomecast-webos-build`); rebuild and push it with
  `tools/push-build-image.sh` after changing the Dockerfile.

The historical JavaScript/browser app, Luna JS service, browser harnesses, and generated
browser WASM bundle were removed. Do not reintroduce Web, MSE, WebCodecs, RDCleanPath, or
browser runtime fallback paths.

## Documentation

- [Documentation index](docs/README.md) — ownership and purpose of each document;
- [native webOS runbook](docs/native-runbook.md) — runtime controls, build, package,
  deploy, acceptance, logging, and triage;
- [build environment](docs/build-environment.md) — reproducible host/container setup;
- [third-party provenance](third_party/PROVENANCE.md) and
  [IronRDP fork provenance](third_party/IronRDP/PROVENANCE.md) — pinned dependencies and
  fork delta.

## License

gnomecast's own code is released under the [MIT License](LICENSE).

Bundled dependencies under `third_party/` keep their own licenses: IronRDP
(MIT OR Apache-2.0), LVGL (MIT), miniaudio (MIT-0), and the IBM Plex and JetBrains Mono
fonts (SIL Open Font License 1.1). The `backend_ndl/`
subproject, including its host-test ABI double, is MIT (see `backend_ndl/THIRD_PARTY.md`).
Packaged builds ship dependency provenance and applicable notices under `licenses/`
inside the `.ipk`.
