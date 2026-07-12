# AGENTS.md

Guidance for AI agents working in this repo. Keep changes minimal, tested, and in the style
of the surrounding code; do not over-engineer.

## Project

`gnomecast` is a native webOS RDP client for LG TVs that casts up to four simultaneous
GNOME desktops (gnome-remote-desktop) to the TV with hardware-decoded video and native
mixed audio. It is a C11/CMake shell (`native/`) around a Rust RDP core (`webrdp-min/`),
linked through a C ABI (`native/include/rdp_ffi.h`).

- **Multi-RDP / HUB**: four RDP session slots map to the remote's red/green/yellow/blue
  buttons. One slot owns the video plane and KVM-style input at a time; the rest stay
  connected in the background (graphics suppressed server-side, audio still mixed). The
  remote's OK button opens the HUB card browser to switch, edit, or connect a slot. See
  `docs/native-runbook.md` for the full navigation/switching/backgrounding model
  (deferred switching, IDR-snapshot backgrounding, the audio mixer overlay).
- **Video**: AVC420/H.264 over RDPEGFX, decoded by the TV hardware video plane via the
  in-house NDL DirectMedia backend (`backend_ndl`, dlopen of `libNDL_directmedia.so.1`).
  Servers without H.264 fall back to native RemoteFX/bitmap decoding in Rust,
  presented as RGBA through SDL. If neither native path works, report a terminal native
  error — never fall back to Web/MSE/WebCodecs/RDCleanPath or the removed browser app.
- **Audio**: MS-RDPEA over `AUDIO_PLAYBACK_DVC`; Opus preferred, 16-bit PCM fallback; a
  headless miniaudio engine mixes all connected slots with adaptive jitter/drift control.
  Strictly best-effort (failures degrade to silent video, never a dropped session).
- **Input**: the USB mouse and keyboard are read from grabbed `/dev/input` (evdev,
  `EVIOCGRAB`) below the compositor; the SDL path is a fallback for the compositor pointer
  (Magic Remote) only. The grab follows window focus so a webOS overlay (TV menu) stays usable.

Stack: C11 + CMake (native shell, decoder boundary, CTests), Rust 2021 (`webrdp-min`
`staticlib` + C ABI), pinned git submodules under `third_party/` (IronRDP fork,
LVGL, miniaudio), and the webOS buildroot toolchain + `ares` CLI for packaging. NDL test
headers live with the standalone backend under `backend_ndl/tests/support/libndl-media/`;
libopus builds from the own recipe in `native/cmake/ExternalOPUS.cmake`.

## Key paths

- `native/src/main.c` — webOS lifecycle, config, RDP callbacks, SDL event loop, presentation,
  and the HUB/multi-slot switching state machine.
- `native/include/ui_host.h`, `ui_slot_palette.h`, `ui_profile_name.h`, `ui_fonts.h` — the
  HUB card browser: layout/navigation, per-slot color palette, profile name editing, and
  the pre-generated IBM Plex/JetBrains Mono font data.
- `native/src/input_evdev.c` / `.h` — unified raw `/dev/input` mouse+keyboard reader (grabbed).
- `native/src/input_sdl.c` / `.h` — RDP fast-path input: window↔desktop coordinate mapping and
  the Linux-keycode / SDL-scancode → RDP scancode maps.
- `native/src/cursor_sdl.c` / `.h` — server-driven cursor on the platform cursor plane.
- `backend_ndl/` — standalone MIT C11 DirectMedia library: public API, dlopen/symbol table,
  atomic combined-track state machine, callbacks, host tests, and installable CMake package.
- `native/include/audio_backend.h`, `media_backend.h`, `video_backend.h` — the adapter
  interface `ndl_adapter/` implements; a future non-NDL backend would implement these instead.
- `native/src/ndl_adapter/` (`audio_ndl.c`, `media_ndl.c`, `video_ndl.c`) — gnomecast-specific
  adapters over `backend_ndl`; own RDP codecs, H.264 framing, keyframe recovery, and
  best-effort audio.
- `native/src/au_snapshot.c` / `.h` — compressed-AU snapshot cache used to re-enter a
  backgrounded session's H.264 delta chain without a visible reconnect (IDR-snapshot
  backgrounding — see the runbook's Multi-RDP section for the trade-offs).
- `native/include/clog.h`, `native/src/category_log.c` — file-scoped category logging:
  one `clog_define(...)` per C file, then lowercase `clog(level, ...)` calls.
- `native/src/rdp_log.c` / `.h` — bridges Rust/IronRDP tracing events into `clog` as the
  `rdp.rust` category, across the synchronous FFI callback.
- `native/src/audio_pipeline.c` / `.h` — headless miniaudio engine, per-source adaptive
  SPSC buffering/resampling, meters, and the NDL PCM pump.
- `native/src/audio_opus.c` / `.h` — Opus decode for RDP audio sources.
- `native/src/luna_volume.c` / `.h` — webOS Luna-bus (`luna://com.webos.audio`) system
  volume bridge for the mixer overlay's MASTER fader; polled, since LS2 volume-change
  subscriptions do not work for a dev-mode app on this device (see the runbook).
- `native/src/h264_annexb.c` — H.264 framing scanners/conversion (AVC length-prefixed
  AND Annex-B — grd sends the latter); the only parsers to use for classifying AUs.
- `native/src/video_rgba_sdl.c` — RemoteFX/bitmap RGBA surface.
- `native/src/ui_preconnect.c` — on-TV LVGL pre-connect settings screen.
- `native/src/ui_mixer.c` / `native/include/ui_mixer.h` — the volume-mixer overlay: dBFS
  faders + live L/R meters (LVGL screen on the preconnect display, plus a raw-SDL fallback
  and the dB gain model).
- `native/include/rdp_ffi.h` — the C↔Rust ABI contract.
- `webrdp-min/src/native.rs` — Rust native worker / C ABI implementation.
- `native/CMakeLists.txt` — native target, options, CTests, webOS package.
- `docs/native-runbook.md` — build/package/deploy/triage; `docs/build-environment.md` —
  host/container toolchain setup; `third_party/PROVENANCE.md` and
  `third_party/IronRDP/PROVENANCE.md` — pinned-dependency provenance and the IronRDP fork delta.

Historical web/browser/Luna trees were removed. Use git history for reference; do not
reintroduce them.

## Commands

Local checks before committing native changes (these mirror CI):

```sh
./tools/syntax-check-native.sh
cargo test --manifest-path webrdp-min/Cargo.toml --features native --locked native::tests::
cmake -S native -B /tmp/gnomecast-native-build && cmake --build /tmp/gnomecast-native-build
ctest --test-dir /tmp/gnomecast-native-build --output-on-failure
```

Add `-DHELLOLG_WITH_OPUS=ON` to the cmake configure to also run the `audio-opus` ctest;
it is off by default on host builds because ExternalOPUS downloads the libopus tarball
(needs network), and defaults to ON only for webOS cross-builds.

The host CMake/ctest build above is SDL-off, so it does not compile `main.c`/`ui_preconnect.c`
or the evdev readers — only the full webOS cross-build (`HELLOLG_WITH_SDL=ON`) does. Cross-build,
package, install, and launch for the TV:

```sh
./tools/build-native-webos.sh
ARES_DEVICE=<tv-device> HELLOLG_NATIVE_CONFIG=native/config.local.json ./tools/deploy-native-webos.sh
```

Use the `armv7-unknown-linux-gnueabi` Rust target, NOT `arm-unknown-linux-gnueabi`: the generic
ARM target can emit CP15 barrier instructions that crash as `Illegal instruction` on ARMv8
webOS. Toolchain default
`/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake`; override with
`WEBOS_TOOLCHAIN_FILE=...`.

## Conventions

- Treat `native/include/rdp_ffi.h` as the ABI contract: a change there must update C, Rust,
  tests, and docs together.
- Callback byte lifetimes are synchronous — copy `on_video_au` / bitmap bytes before returning
  if you retain them.
- RDPEGFX AVC data arrives in BOTH framings: nominally length-prefixed H.264, but
  gnome-remote-desktop delivers Annex-B directly. Feed the NDL backend Annex-B (convert or
  pass through); classify AUs only with the `h264_annexb.h` scanners, which handle both.
- Product webOS builds set `HELLOLG_WITH_NDL=ON`, `HELLOLG_WITH_SDL=ON`, and
  `HELLOLG_LINK_RDP_FFI=ON`. The binary dlopens `libNDL_directmedia.so.1` at runtime — it
  must never carry a DT_NEEDED on it (verify_package_root enforces this).
- C: C11, 4-space indent, small file-local helpers, explicit error returns, ASCII. Native
  symbols use `native_*` / subsystem prefixes; exported ABI symbols use `rdp_*`. Rust: `cargo
  fmt`. Shell: `#!/usr/bin/env bash` + `set -euo pipefail`.
- Every production C translation unit under `native/src/` uses exactly one file-scoped
  `clog_define(...)` and at least one `clog(...)` or rate-limited `clog_limited(...)` call.
  Exceptions are the `category_log.c` logger engine and generated font sources. The standalone
  `backend_ndl` library keeps its callback logger; the native adapter forwards it as
  `media.ndl`.
- Do not bypass `clog` with direct stderr writes, `perror`, `SDL_Log`, Rust `eprintln!`, or a
  stderr tracing subscriber. Rust logging crosses the synchronous RDP callback bridge as
  `rdp.rust`. `GNOMECAST_LOG` is the only runtime level control; do not restore
  `WEBRDP_LOG`, `GNOMECAST_NDL_LOG`, or `/tmp/gnomecast-ndl-debug`.
- Do not log per-frame, per-pointer-motion, or per-audio-block success. Put noisy diagnostics
  below `info` and rate-limit repeated hot-path failures with `clog_limited`.
- Keep generated artifacts out of the repo (`native/config.local.json`, build dirs, logs,
  `*.ipk`).

## Git / release workflow

- Dev history lives in Bitbucket (`origin`, `kodavr/gnomecast`); pushes auto-trigger CI on
  self-hosted runners. `main` is protected — land changes via `bkt pr` (create → `bkt pr checks
  --wait` → `bkt pr merge`), not direct pushes.
- GitHub (`truebest/gnomecast`) is an append-only, releases-only mirror: one snapshot commit +
  `vX.Y.Z` tag per version, published with `tools/release-github.sh <version>`. Never
  force-push or move tags there.
- Bump `native/deploy/webos/appinfo.json` `version` for a release. Never add Claude/AI
  attribution trailers to commit messages.

## Do not

- Do not reintroduce Web/MSE/WebCodecs/RDCleanPath or the deleted browser/Luna trees, and do
  not package `app/` or `service/` into the native target.
- Do not substitute a software H.264 decoder for acceptance; the NDL hardware path is
  required on the TV.
- Do not log passwords or local config contents.
