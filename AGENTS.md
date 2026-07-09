# AGENTS.md

Guidance for AI agents working in this repo. Keep changes minimal, tested, and in the style
of the surrounding code; do not over-engineer.

## Project

`gnomecast` is a native webOS RDP client for LG TVs that casts a GNOME desktop
(gnome-remote-desktop) to the TV with hardware-decoded video and audio. It is a C11/CMake
shell (`native/`) around a Rust RDP core (`webrdp-min/`), linked through a C ABI
(`native/include/rdp_ffi.h`).

- **Video**: AVC420/H.264 over RDPEGFX, decoded by the TV hardware video plane via `ss4s`
  (NDL/SMP). Servers without H.264 fall back to native RemoteFX/bitmap decoding in Rust,
  presented as RGBA through SDL. If neither native path works, report a terminal native
  error — never fall back to Web/MSE/WebCodecs/RDCleanPath or the removed browser app.
- **Audio**: MS-RDPEA over `AUDIO_PLAYBACK_DVC`; Opus preferred, 16-bit PCM fallback;
  strictly best-effort (failures degrade to silent video, never a dropped session).
- **Input**: the USB mouse and keyboard are read from grabbed `/dev/input` (evdev,
  `EVIOCGRAB`) below the compositor; the SDL path is a fallback for the compositor pointer
  (Magic Remote) only. The grab follows window focus so a webOS overlay (TV menu) stays usable.

Stack: C11 + CMake (native shell, decoder boundary, CTests), Rust 2021 (`webrdp-min`
`staticlib` + C ABI), vendored deps under `third_party/` (IronRDP fork, ss4s, commons, LVGL),
and the webOS buildroot toolchain + `ares` CLI for packaging.

## Key paths

- `native/src/main.c` — webOS lifecycle, config, RDP callbacks, SDL event loop, presentation.
- `native/src/input_evdev.c` / `.h` — unified raw `/dev/input` mouse+keyboard reader (grabbed).
- `native/src/input_sdl.c` / `.h` — RDP fast-path input: window↔desktop coordinate mapping and
  the Linux-keycode / SDL-scancode → RDP scancode maps.
- `native/src/cursor_sdl.c` / `.h` — server-driven cursor on the platform cursor plane.
- `native/src/media_ss4s.c`, `video_ss4s.c`, `audio_ss4s.c` — the shared ss4s player and its
  video/audio tracks.
- `native/src/h264_annexb.c` — AVC length-prefixed H.264 → Annex-B conversion.
- `native/src/video_rgba_sdl.c` — RemoteFX/bitmap RGBA surface.
- `native/src/ui_preconnect.c` — on-TV LVGL pre-connect settings screen.
- `native/src/ui_mixer.c` / `.h` — the volume-mixer overlay: dBFS faders + live L/R meters
  (LVGL screen on the preconnect display, plus a raw-SDL fallback and the dB gain model).
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
cc -fsyntax-only -Inative/include \
  native/src/main.c native/src/media_ss4s.c native/src/video_ss4s.c \
  native/src/audio_ss4s.c native/src/audio_mixer.c native/src/audio_opus.c \
  native/src/video_rgba_sdl.c \
  native/src/h264_annexb.c native/src/input_sdl.c native/src/cursor_sdl.c native/src/ui_mixer.c \
  native/src/rdp_ffi_stub.c native/src/config_paths.c native/src/settings_json.c
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
- RDPEGFX AVC data is length-prefixed H.264; normalize to Annex-B before feeding ss4s.
- Product webOS builds set `HELLOLG_WITH_SS4S=ON`, `HELLOLG_WITH_SDL=ON`, and
  `HELLOLG_LINK_RDP_FFI=ON`.
- C: C11, 4-space indent, small file-local helpers, explicit error returns, ASCII. Native
  symbols use `native_*` / subsystem prefixes; exported ABI symbols use `rdp_*`. Rust: `cargo
  fmt`. Shell: `#!/usr/bin/env bash` + `set -euo pipefail`.
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
- Do not use dummy/software ss4s video backends for acceptance.
- Do not log passwords or local config contents.
