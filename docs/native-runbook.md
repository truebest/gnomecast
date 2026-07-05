# Native ss4s Runbook

This runbook tracks the current native-only webOS target. The native app must not fall
back to a web app, JavaScript service, MSE, WebCodecs, RDCleanPath, or browser rendering.
AVC420/H.264 is preferred through ss4s/NDL/SMP; RemoteFX is supported only as a native
Rust/IronRDP decode path with RGBA updates presented by SDL.

## Local Config

Local native config lives at `native/config.local.json`. It is gitignored and may contain
passwords. Do not commit it or paste it into logs.

Example shape:

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

The pre-connect UI saves the last successful Connect settings, including address,
username, domain, password, and FPS, into the app writable SDL preferences directory.
Those saved settings are loaded on the next start, and explicit webOS launch params or
CLI flags override them. Set `HELLOLG_IGNORE_SAVED_CONFIG=1` for a one-off launch that
ignores saved UI settings.

## Local Build And Test

Targeted local loop:

```sh
cc -fsyntax-only -Inative/include \
  native/src/main.c native/src/media_ss4s.c native/src/video_ss4s.c \
  native/src/audio_ss4s.c native/src/video_rgba_sdl.c \
  native/src/h264_annexb.c native/src/input_sdl.c native/src/cursor_sdl.c \
  native/src/rdp_ffi_stub.c
cargo test --manifest-path webrdp-min/Cargo.toml --features native native::tests::
cmake -S native -B /tmp/gnomecast-native-build-tests
cmake --build /tmp/gnomecast-native-build-tests
ctest --test-dir /tmp/gnomecast-native-build-tests --output-on-failure
```

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

Not yet implemented or not yet TV-verified:

- Native RemoteFX/bitmap fallback path against a server without H.264.
- Live fullscreen RemoteFX-only/native RGBA presentation on TV.

## Third-Party Provenance

Initialize pinned native dependencies:

```sh
git submodule update --init third_party/IronRDP third_party/ss4s third_party/commons third_party/lvgl
```

See `third_party/PROVENANCE.md` for pinned commits, licenses, and the Moonlight reference boundary.
