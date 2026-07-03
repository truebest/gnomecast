# Native gnomecast build environment for local and CI builds.
#
# Build:
#   docker build -t gnomecast-native-build .
# Use:
#   docker run --rm -v "$PWD:/workspace" -w /workspace gnomecast-native-build \
#     bash -lc './tools/build-native-webos.sh'

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG RUST_TOOLCHAIN=stable
ARG NODE_VERSION=20.20.2
ARG WEBOS_CLI_VERSION=3.2.4
ARG WEBOS_TOOLCHAIN_URL=https://github.com/openlgtv/buildroot-nc4/releases/latest/download/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz
ARG WEBOS_TOOLCHAIN_DIR=/opt/arm-webos-linux-gnueabi_sdk-buildroot

ENV CARGO_HOME=/opt/cargo \
    RUSTUP_HOME=/opt/rustup \
    PATH=/opt/cargo/bin:/usr/local/bin:/opt/arm-webos-linux-gnueabi_sdk-buildroot/bin:${PATH} \
    WEBOS_TOOLCHAIN_FILE=/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake \
    NATIVE_WEBOS_RUST_TARGET=armv7-unknown-linux-gnueabi

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      xz-utils \
      tar \
      git \
      openssh-client \
      build-essential \
      cmake \
      pkg-config \
      python3 \
      file \
      binutils \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    arch="$(uname -m)"; \
    case "$arch" in \
      x86_64) node_arch="x64" ;; \
      aarch64|arm64) node_arch="arm64" ;; \
      *) echo "unsupported Node architecture: $arch" >&2; exit 1 ;; \
    esac; \
    curl -fsSLo /tmp/node.tar.xz "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-${node_arch}.tar.xz"; \
    tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1; \
    rm -f /tmp/node.tar.xz; \
    node --version; \
    npm --version; \
    npm install -g "@webos-tools/cli@${WEBOS_CLI_VERSION}"; \
    ares-package --version || true

RUN set -eux; \
    curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal --default-toolchain "${RUST_TOOLCHAIN}"; \
    rustup component add rustfmt; \
    rustup target add "${NATIVE_WEBOS_RUST_TARGET}"; \
    rustc --version; \
    cargo --version

RUN set -eux; \
    mkdir -p /opt; \
    curl -fL --retry 3 -o /tmp/webos-toolchain.tar.gz "${WEBOS_TOOLCHAIN_URL}"; \
    tar -xzf /tmp/webos-toolchain.tar.gz -C /opt; \
    rm -f /tmp/webos-toolchain.tar.gz; \
    test -d "${WEBOS_TOOLCHAIN_DIR}"; \
    "${WEBOS_TOOLCHAIN_DIR}/relocate-sdk.sh" || true; \
    test -f "${WEBOS_TOOLCHAIN_FILE}"; \
    # The SDK bundles host cmake/ctest/cpack linked against libssl1.1, which ubuntu:24.04
    # no longer ships; the SDK bin dir precedes /usr/bin in PATH, so they would shadow the
    # apt-installed cmake and fail to start. The cross toolchain only needs the
    # toolchainfile, not the SDK's own cmake binaries.
    rm -f "${WEBOS_TOOLCHAIN_DIR}/bin/cmake" "${WEBOS_TOOLCHAIN_DIR}/bin/ctest" "${WEBOS_TOOLCHAIN_DIR}/bin/cpack"; \
    cmake --version | head -n 1; \
    "${WEBOS_TOOLCHAIN_DIR}/bin/arm-webos-linux-gnueabi-gcc" --version | head -n 1

WORKDIR /workspace

CMD ["bash"]
