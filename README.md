# gnomecast

`gnomecast` is a native webOS RDP client for LG TVs, built to cast a GNOME desktop
(gnome-remote-desktop) to the TV with hardware decoding for both video and audio.

## Features

- **Video**: AVC420/H.264 over RDPEGFX, decoded by the TV hardware video plane through
  ss4s (NDL/SMP). Servers that cannot provide H.264 fall back to native RemoteFX
  Progressive/bitmap decoding in Rust, presented as RGBA through SDL.
- **Audio**: MS-RDPEA over the `AUDIO_PLAYBACK_DVC` dynamic channel (the transport
  gnome-remote-desktop uses). Opus 48 kHz stereo is negotiated preferentially and
  hardware-decoded by the TV; 16-bit PCM is the fallback. Audio is strictly best-effort:
  failures degrade to silent video, never to a dropped session.
- **Input**: keyboard, unicode text, absolute/relative mouse, and scroll wheel mapped from
  SDL/webOS (including magic-remote specifics) to RDP fast-path input.
- **Pre-connect UI**: an on-TV LVGL settings screen (host, credentials, fps, mouse mode)
  with persisted settings.
- **Network autodetection**: the client answers connect-time and continuous RTT
  measurements over the MCS message channel — gnome-remote-desktop refuses audio
  redirection without it.

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
- `Dockerfile` + `bitbucket-pipelines.yml` — reproducible build container and CI that
  produces the webOS `.ipk`.

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
