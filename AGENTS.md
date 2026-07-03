You are an experienced, pragmatic software engineering AI agent. Keep edits minimal and do not over-engineer.

# Agent Guide

## Project Overview

`gnomecast` is moving to a **native-only webOS app** on branch `integration/native-e2e`. The native target lives under `native/` and uses the Rust RDP core in `webrdp-min/` through a C ABI.

The native product path prefers AVC420/H.264 through the webOS hardware video plane via `ss4s` / NDL / SMP. RemoteFX is also required as a native compatibility path for servers without H.264: Rust/IronRDP decodes bitmap updates and the native SDL presenter displays RGBA frames. If neither native graphics path can work, report a terminal native error. Do not add runtime fallback to Web, MSE, WebCodecs, RDCleanPath, or the historical browser app.

Main technologies:

- C11 and CMake for the native shell, decoder boundary, input helpers, package targets, and CTest tests.
- Rust 2021 for `webrdp-min` and the native `staticlib` C ABI.
- Vendored/pinned dependencies under `third_party/`, including IronRDP, ss4s, and commons.
- webOS CLI and webOS buildroot toolchain for package/install/launch work.

## Important Paths

- `docs/native-runbook.md` - current build, package, install, launch, and triage commands.
- `docs/build-environment.md` - host/container setup notes.
- `native/CMakeLists.txt` - native executable, tests, ss4s/SDL/staticlib options, and webOS package targets.
- `native/include/rdp_ffi.h` - C ABI contract between native C and Rust.
- `native/src/main.c` - native lifecycle, config, callbacks, SDL/webOS event loop.
- `native/src/video_ss4s.c` and `native/include/video_ss4s.h` - ss4s H.264 decoder boundary.
- `native/src/h264_annexb.c` and `native/include/h264_annexb.h` - AVC length-prefixed H.264 to Annex-B conversion.
- `native/src/input_sdl.c` and `native/include/input_sdl.h` - native input mapping.
- `native/src/video_rgba_sdl.c` and `native/include/video_rgba_sdl.h` - native RGBA/RemoteFX bitmap surface helper.
- `native/tests/` - CTest coverage for ABI, H.264, and input helpers.
- `webrdp-min/src/native.rs` - Rust native C ABI implementation.
- `third_party/PROVENANCE.md` - pinned third-party dependency provenance and licenses.

Historical web/browser/runtime trees were removed. Use git history for reference; do not reintroduce browser runtime fallback.

## Essential Commands

Run focused local checks before committing native changes:

```sh
cc -fsyntax-only -Inative/include \
  native/src/main.c native/src/video_ss4s.c native/src/video_rgba_sdl.c \
  native/src/h264_annexb.c native/src/input_sdl.c native/src/rdp_ffi_stub.c
cargo test --manifest-path webrdp-min/Cargo.toml --features native native::tests::
```

Local CMake test loop:

```sh
cmake -S native -B /tmp/gnomecast-native-build
cmake --build /tmp/gnomecast-native-build
ctest --test-dir /tmp/gnomecast-native-build --output-on-failure
```

Build Rust staticlib and link it into the native shell:

```sh
cargo build --manifest-path webrdp-min/Cargo.toml --features native
cmake -S native -B /tmp/gnomecast-native-rust-build \
  -DHELLOLG_LINK_RDP_FFI=ON \
  -DRDP_FFI_LIB="$PWD/webrdp-min/target/debug/libwebrdp_min.a"
cmake --build /tmp/gnomecast-native-rust-build
```

Cross-build, package, install, and launch for webOS:

```sh
./tools/build-native-webos.sh
ARES_DEVICE=<tv-device> HELLOLG_NATIVE_CONFIG=native/config.local.json ./tools/deploy-native-webos.sh
```

Do not use `arm-unknown-linux-gnueabi` for the TV Rust staticlib. Use
`armv7-unknown-linux-gnueabi`; the generic ARM target can emit CP15 barrier instructions
that crash as `Illegal instruction` on ARMv8 webOS.

Default webOS toolchain path:

```text
/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
```

Override as needed:

```sh
WEBOS_TOOLCHAIN_FILE=/path/to/toolchainfile.cmake ./tools/build-native-webos.sh
```

## Patterns

- Keep native work small and testable: helpers in `native/src/`, public headers in `native/include/`, focused tests in `native/tests/`.
- Treat `native/include/rdp_ffi.h` as the ABI contract. If it changes, update C, Rust, tests, and docs together.
- Callback byte lifetimes are synchronous. C must copy `on_video_au` data if it needs to retain it.
- RDPEGFX AVC data is length-prefixed H.264. Normalize to Annex-B before feeding ss4s.
- RemoteFX/bitmap updates are native RGBA dirty rectangles; copy callback bytes synchronously into the native bitmap surface before returning.
- Product webOS builds should use `HELLOLG_WITH_SS4S=ON`, `HELLOLG_WITH_SDL=ON`, and `HELLOLG_LINK_RDP_FFI=ON`.
- Keep generated artifacts out of the repo: `native/config.local.json`, build dirs, logs, and `*.ipk` outputs are ignored.

## Anti-Patterns

- Do not package `app/` or `service/` into the native target.
- Do not use dummy/software ss4s video backends for MVP acceptance.
- Do not copy or adapt Moonlight TV (https://github.com/mariotaku/moonlight-tv) GPL application code — gnomecast is MIT-licensed. Use it strictly as a read-only reference and preserve dependency licenses when pinning submodules.
- Do not log passwords or local config contents.
- Do not recreate deleted browser harnesses unless the native binary implements the corresponding runtime mode in the same change.

## Code Style

- C uses C11, 4-space indentation, small file-local helpers, explicit error returns, and standard headers.
- Rust uses `cargo fmt`.
- Shell scripts use `#!/usr/bin/env bash` and `set -euo pipefail`.
- Prefer ASCII in new files unless there is a clear reason otherwise.
- Match existing naming: native C symbols use `native_*` or subsystem prefixes; exported RDP ABI symbols use `rdp_*`.

## Validation

No repo-wide formatter or lint script is configured. Prefer the smallest relevant checks:

```sh
cc -fsyntax-only -Inative/include \
  native/src/main.c native/src/video_ss4s.c native/src/video_rgba_sdl.c \
  native/src/h264_annexb.c native/src/input_sdl.c native/src/rdp_ffi_stub.c
cargo test --manifest-path webrdp-min/Cargo.toml --features native native::tests::
cmake -S native -B /tmp/gnomecast-native-build
cmake --build /tmp/gnomecast-native-build
ctest --test-dir /tmp/gnomecast-native-build --output-on-failure
```

If a tool is unavailable or an existing unrelated warning blocks a check, report that explicitly.

## Current Native Status

Implemented:

- Native CMake target and native `appinfo.json` for `com.truebest.gnomecast.native`.
- Frozen C ABI and Rust `native` feature/staticlib output.
- Direct Rust native worker scaffold with state/log/video callbacks.
- C FFI stub for local scaffold builds, while product webOS builds require the Rust staticlib.
- ss4s/commons submodule pinning and license/provenance staging.
- ss4s NDL/SMP module selection, dummy backend rejection, and H.264 feed boundary.
- H.264 AVC-to-Annex-B normalization with AU size caps and CTests.
- ABI layout and input helper CTests.
- SDL/webOS fullscreen event loop and input dispatch in product builds.
- Native build/package/verify and deploy/launch helper scripts.
- Native package/install/launch verification on the TV.
- Live fullscreen GNOME desktop through the ss4s/NDL/SMP hardware plane, TV-verified
  (`ndl-webos5`/AVC420), including correct behavior when the server's real EGFX surface
  resolution differs from the negotiated MCS/GCC desktop size.

Still pending or not TV-verified:

- Native RemoteFX/bitmap fallback path against a server without H.264.
