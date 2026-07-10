# Build Environment Setup

This note is for setting up `gnomecast` on a new machine or container for the native-only webOS target.

## Base Linux Packages

```sh
sudo apt install -y git build-essential cmake pkg-config curl
```

Required commands:

- `git`
- `cc`
- `cmake`
- `ctest`
- `pkg-config`

## Node And webOS CLI

Node/npm are used for the webOS CLI, not for product app code.

```sh
curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.5/install.sh | bash
source ~/.bashrc
nvm install 22
nvm use 22
nvm alias default 22
npm install -g @webos-tools/cli@3.2.4
```

Expected commands:

```sh
ares-package
ares-install
ares-launch
ares-setup-device
ares-inspect
ares-device-info
```

## Rust

Install Rust through `rustup`:

```sh
rustup default stable
```

## Repository Dependencies

From the repository root:

```sh
git submodule update --init third_party/IronRDP third_party/ss4s third_party/commons third_party/lvgl third_party/miniaudio
```

Pinned native third-party revisions are recorded in `third_party/PROVENANCE.md`.

Run the local native smoke commands:

```sh
cc -fsyntax-only -Inative/include -Ithird_party/miniaudio \
  native/src/main.c native/src/media_ss4s.c native/src/video_ss4s.c \
  native/src/audio_ss4s.c native/src/audio_pipeline.c native/src/audio_opus.c \
  native/src/video_rgba_sdl.c \
  native/src/h264_annexb.c native/src/input_sdl.c native/src/cursor_sdl.c \
  native/src/rdp_ffi_stub.c native/src/config_paths.c native/src/settings_json.c
cargo test --manifest-path webrdp-min/Cargo.toml --features native native::tests::
cmake -S native -B /tmp/gnomecast-native-build
cmake --build /tmp/gnomecast-native-build
ctest --test-dir /tmp/gnomecast-native-build --output-on-failure
```

## webOS Native Cross-Build

The native webOS product cross-build requires:

- webOS buildroot toolchain;
- SDL2 for the target;
- initialized `third_party/IronRDP`, `third_party/ss4s`, `third_party/commons`,
  `third_party/lvgl`, and `third_party/miniaudio`;
- Rust target support for `armv7-unknown-linux-gnueabi`.

Default toolchain path expected by `tools/build-native-webos.sh`:

```text
/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
```

If installed elsewhere:

```sh
WEBOS_TOOLCHAIN_FILE=/path/to/toolchainfile.cmake ./tools/build-native-webos.sh
```

Do not use the generic Rust target `arm-unknown-linux-gnueabi` for TV packages; it can emit legacy CP15 barrier instructions that crash as `Illegal instruction` on ARMv8 webOS.

Deploy and launch:

```sh
ARES_DEVICE=<tv-device> HELLOLG_NATIVE_CONFIG=native/config.local.json \
  ./tools/deploy-native-webos.sh
```

## Minimal Local Verification

```sh
git submodule update --init third_party/IronRDP third_party/ss4s third_party/commons third_party/lvgl third_party/miniaudio
cmake -S native -B /tmp/gnomecast-native-build
cmake --build /tmp/gnomecast-native-build
ctest --test-dir /tmp/gnomecast-native-build --output-on-failure
```
