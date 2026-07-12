# Build environment

The native webOS build is reproducible through the repository
[Dockerfile](../Dockerfile). CI uses the prebuilt image pinned in
[bitbucket-pipelines.yml](../bitbucket-pipelines.yml); those two files are the source of
truth for tool versions and system dependencies.

For package, install, launch, and device-triage procedures, see the
[native webOS runbook](native-runbook.md).

## Recommended: container build

After initializing the repository dependencies as described below, build the environment
locally and run the product build from the repository root:

```sh
docker build -t gnomecast-webos-build .
docker run --rm -v "$PWD:/workspace" -w /workspace \
  gnomecast-webos-build bash -lc './tools/build-native-webos.sh'
```

The Dockerfile installs the webOS buildroot toolchain, target SDL2 support, a static
libevdev 1.13.6 in the target sysroot, Rust, Node, and the webOS CLI. CI avoids rebuilding
that environment on every run by using the public image named in
`bitbucket-pipelines.yml`.

## Manual host setup

The reference environment is Ubuntu 24.04 with these host packages:

```sh
sudo apt update
sudo apt install -y --no-install-recommends \
  ca-certificates curl xz-utils tar git openssh-client build-essential \
  cmake pkg-config python3 file binutils
```

Install the remaining tools at the versions used by the Dockerfile:

- stable Rust through `rustup`, including `rustfmt`;
- Rust target `armv7-unknown-linux-gnueabi`;
- Node.js 20.20.2 and `@webos-tools/cli` 3.2.4;
- the openlgtv `arm-webos-linux-gnueabi` buildroot SDK used by the Dockerfile;
- target SDL2 and static libevdev in that SDK's sysroot.

The Rust setup is:

```sh
rustup toolchain install stable --profile minimal
rustup default stable
rustup component add rustfmt
rustup target add armv7-unknown-linux-gnueabi
```

After installing Node.js 20.20.2:

```sh
npm install -g @webos-tools/cli@3.2.4
ares-package --version
```

For a manual SDK installation, follow the download, relocation, and libevdev sysroot
steps in the Dockerfile rather than maintaining a second recipe here. The build helper
looks for the toolchain file in this order:

```text
$WEBOS_TOOLCHAIN_FILE
$HOME/.local/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
```

## Repository setup

Initialize the pinned dependencies from the repository root:

```sh
git submodule update --init third_party/IronRDP third_party/lvgl third_party/miniaudio
```

Their pinned revisions and licenses are recorded in
[third-party provenance](../third_party/PROVENANCE.md).

## Verify the setup

Run the canonical local checks from the
[native webOS runbook](native-runbook.md#local-build-and-test), then complete one full
cross-build. The host CMake build does not exercise the webOS SDL/LVGL/evdev product
paths, so it cannot validate the environment by itself.

## webOS cross-build

With the SDK and webOS CLI available, build and package the application with:

```sh
./tools/build-native-webos.sh
```

If the SDK is installed elsewhere:

```sh
WEBOS_TOOLCHAIN_FILE=/path/to/toolchainfile.cmake ./tools/build-native-webos.sh
```

The script builds the Rust static library, configures the required NDL/SDL/LVGL/RDP-FFI
product options, verifies the staged native-only package, and writes the IPK under
`dist/native-webos/`.

Always use the Rust target `armv7-unknown-linux-gnueabi`. The generic
`arm-unknown-linux-gnueabi` target can emit CP15 barrier instructions that fail with
`Illegal instruction` on ARMv8 webOS devices, and the build helper rejects it.
