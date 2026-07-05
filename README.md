# gnomecast

`gnomecast` is a native webOS RDP client for LG TVs, built to cast a GNOME desktop
(gnome-remote-desktop) to the TV with hardware decoding for both video and audio.

## Features

- **Video**: AVC420/H.264 over RDPEGFX, decoded by the TV hardware video plane through
  ss4s (NDL/SMP). Servers that cannot provide H.264 fall back to native RemoteFX
  Progressive/bitmap decoding in Rust, presented as RGBA through SDL.
- **Audio**: MS-RDPEA over the `AUDIO_PLAYBACK_DVC` dynamic channel (the transport
  gnome-remote-desktop uses). Opus 48 kHz stereo is negotiated preferentially and
  hardware-decoded by the TV; 16-bit PCM is the fallback. A configurable jitter prebuffer
  (0–300 ms slider, default 100 ms) smooths bursty delivery. Audio is strictly best-effort:
  failures degrade to silent video, never to a dropped session.
- **Input**: keyboard, unicode text, absolute/relative mouse, and scroll wheel mapped from
  SDL/webOS (including magic-remote specifics) to RDP fast-path input. NumLock is synced on
  connect so an attached keyboard's numpad types digits, and a toggleable jump filter
  cancels the webOS IME pointer-recenter warp so the cursor stays put while typing.
- **Server cursor**: the real pointer shapes (I-beam, resize arrows, ...) arrive as RDP
  pointer updates and are applied as the system color cursor, composited by the platform's
  cursor plane above the hardware video with zero extra presents.
- **Pre-connect UI**: an on-TV LVGL settings screen (host, credentials, fps, mouse mode)
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
- **Server cursor shapes require absolute mouse mode.** In relative mode (Magic Remote)
  SDL hides the system cursor, so the server's pointer shapes cannot be shown there.
- **AVC444 is not negotiated** — the client advertises AVC420 (4:2:0) only. Servers
  without a usable H.264 encoder fall back to software RemoteFX rendering (slower, and
  rendered on the ~1080p UI plane rather than the native-resolution video plane).
- **EGFX surface-composition operations are ignored** (SolidFill, SurfaceToSurface,
  CacheToSurface). Harmless with gnome-remote-desktop — it sends SurfaceToSurface during
  routine AVC420 sessions and the picture is complete since the video plane carries full
  frames — but a server that relied on them for the software RemoteFX path would show
  stale regions.
- **The pointer may briefly flash at the screen center while typing.** The webOS
  compositor recenters the pointer around IME show/hide transitions and there is no
  setting to disable it. The client filters the jump out of the input stream (the
  "Jump filter" toggle in the settings screen), so the remote cursor position is
  unaffected; the local flash is cosmetic.

## Layout

- `native/` — C11/CMake shell: webOS lifecycle, SDL input and presentation, the shared
  ss4s media player (`media_ss4s.c`) with its video (`video_ss4s.c`) and audio
  (`audio_ss4s.c`) tracks, RemoteFX RGBA presentation, pre-connect UI, package targets.
- `webrdp-min/` — Rust static library implementing the RDP client (direct TCP + TLS +
  CredSSP/NTLM, EGFX, rdpsnd-over-DVC), exposed through the C ABI in
  `native/include/rdp_ffi.h`.
- `third_party/` — pinned native dependencies (git submodules): our IronRDP fork
  ([truebest/IronRDP](https://github.com/truebest/IronRDP), branch `gnome-rdp-support`,
  mirror patch record in `patches/ironrdp/`), ss4s, commons, LVGL.
- `Dockerfile` + `bitbucket-pipelines.yml` — reproducible build environment and CI that
  produces the webOS `.ipk`. CI runs inside a prebuilt public image
  (`cubicattache/gnomecast-webos-build`); rebuild and push it with
  `tools/push-build-image.sh` after changing the Dockerfile.

The historical JavaScript/browser app, Luna JS service, browser harnesses, and generated
browser WASM bundle were removed. Do not reintroduce Web, MSE, WebCodecs, RDCleanPath, or
browser runtime fallback paths.

## Documentation

- `docs/native-runbook.md` — build/package/deploy/triage commands;
- `docs/build-environment.md` — host and webOS toolchain setup;
- `docs/audio-rdpsnd-design-notes.md` — audio design record: protocol findings
  (gnome-remote-desktop specifics), codec strategy, and live bring-up gotchas;
- `third_party/PROVENANCE.md` and `third_party/IronRDP/PROVENANCE.md` — pinned dependency
  provenance and the IronRDP fork delta.

## License

gnomecast's own code is released under the [MIT License](LICENSE).

Bundled dependencies under `third_party/` (git submodules) keep their own licenses: IronRDP
(MIT OR Apache-2.0), LVGL (MIT), commons (MIT), and **ss4s (LGPL-3.0)**. The ss4s core is
statically linked into the application binary; the LGPL-3.0 relinking requirement is
satisfied by this repository being fully open source. Packaged builds ship all dependency
license texts under `licenses/` inside the `.ipk`.
