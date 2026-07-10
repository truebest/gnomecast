#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${NATIVE_WEBOS_BUILD_DIR:-$repo_root/build/native-webos}"
stage_dir="${NATIVE_WEBOS_STAGING_DIR:-$build_dir/stage}"
dist_dir="${NATIVE_WEBOS_DIST_DIR:-$repo_root/dist/native-webos}"
# Size-optimized by default, but with -g kept (see CMAKE_*_FLAGS_MINSIZEREL below) so on-device
# crashes stay debuggable. Override with NATIVE_WEBOS_BUILD_TYPE=RelWithDebInfo for -O2, etc.
build_type="${NATIVE_WEBOS_BUILD_TYPE:-MinSizeRel}"
target="${NATIVE_WEBOS_RUST_TARGET:-armv7-unknown-linux-gnueabi}"
if [[ "$target" != "armv7-unknown-linux-gnueabi" ]]; then
  echo "build-native-webos: native webOS packages require NATIVE_WEBOS_RUST_TARGET=armv7-unknown-linux-gnueabi (got $target)" >&2
  exit 2
fi

rust_profile="${NATIVE_WEBOS_RUST_PROFILE:-release}"
skip_build="${NATIVE_WEBOS_SKIP_BUILD:-0}"

fail() {
  echo "build-native-webos: $*" >&2
  exit 2
}

find_toolchain() {
  if [[ -n "${WEBOS_TOOLCHAIN_FILE:-}" ]]; then
    printf '%s\n' "$WEBOS_TOOLCHAIN_FILE"
    return
  fi
  for candidate in \
    "$HOME/.local/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake" \
    "/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  printf '%s\n' "/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake"
}

verify_package_root() {
  local root="$1"

  [[ -f "$root/appinfo.json" ]] || fail "missing appinfo.json in staged package"
  [[ -f "$root/bin/gnomecast-native" ]] || fail "missing bin/gnomecast-native in staged package"
  [[ -f "$root/icon.png" ]] || fail "missing icon.png in staged package"

  python3 - "$root/appinfo.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
with path.open("r", encoding="utf-8") as f:
    appinfo = json.load(f)
expected = {
    "id": "com.truebest.gnomecast.native",
    "type": "native",
    "main": "bin/gnomecast-native",
    "icon": "icon.png",
}
for key, value in expected.items():
    actual = appinfo.get(key)
    if actual != value:
        raise SystemExit(f"appinfo.json {key!r} is {actual!r}, expected {value!r}")
PY

  command -v strings >/dev/null 2>&1 || fail "strings is required to inspect native binary markers"
  if grep -Fq "native RDP FFI stub" < <(strings "$root/bin/gnomecast-native"); then
    fail "native binary was linked with the C RDP FFI stub; product packages require the Rust staticlib"
  fi
  if grep -Fq "SDL event loop is not compiled in" < <(strings "$root/bin/gnomecast-native"); then
    fail "native binary was built without SDL; product packages require HELLOLG_WITH_SDL=ON"
  fi
  [[ -d "$root/lib" ]] || fail "missing lib/ with packaged ss4s hardware modules"
  [[ -f "$root/lib/ss4s_modules.ini" ]] || fail "missing lib/ss4s_modules.ini"
  local hardware_module_match
  hardware_module_match="$(find "$root/lib" -maxdepth 1 -type f \
    \( -name 'ss4s-ndl-webos5*.so*' -o -name 'ss4s-ndl-webos4*.so*' -o -name 'ss4s-ndl-esplayer*.so*' -o -name 'ss4s-smp-webos*.so*' \) \
    -print -quit)"
  [[ -n "$hardware_module_match" ]] || fail "missing allowed ss4s webOS hardware module under lib/"

  for forbidden in app service; do
    [[ ! -e "$root/$forbidden" ]] || fail "forbidden historical runtime tree included: $forbidden/"
  done

  local web_runtime_match
  web_runtime_match="$(find "$root" -type f \( -name '*.html' -o -name '*.js' -o -name 'package.json' \) -print -quit)"
  [[ -z "$web_runtime_match" ]] || fail "web/browser runtime file included: ${web_runtime_match#$root/}"

  local dummy_match
  dummy_match="$(find "$root" -type f \( -name '*ss4s-dummy*' -o -name '*dummy*' \) -print -quit)"
  [[ -z "$dummy_match" ]] || fail "dummy/software ss4s backend included: ${dummy_match#$root/}"

  if [[ -d "$root/lib" && -n "$(find "$root/lib" -maxdepth 1 -type f -name 'ss4s-*.so*' -print -quit)" ]]; then
    command -v readelf >/dev/null 2>&1 || fail "readelf is required to verify ss4s module RUNPATH"
    local dynamic_tags
    dynamic_tags="$(readelf -d "$root/bin/gnomecast-native")" || fail "failed to inspect native binary dynamic tags"
    if ! grep -Eq '\((RPATH|RUNPATH)\).*\$ORIGIN/\.\./lib' <<<"$dynamic_tags"; then
      fail "native binary RUNPATH/RPATH must include \$ORIGIN/../lib so ss4s can dlopen packaged modules"
    fi
  fi

  echo "build-native-webos: package verify OK ($root)"
}

verify_ipk() {
  local ipk="$1"
  local cleanup
  cleanup="$(mktemp -d)"
  trap 'rm -rf "$cleanup"' RETURN
  mkdir -p "$cleanup/ar" "$cleanup/root"
  (cd "$cleanup/ar" && ar x "$ipk")
  local data_tar
  data_tar="$(find "$cleanup/ar" -maxdepth 1 -type f -name 'data.tar*' -print -quit)"
  [[ -n "$data_tar" ]] || fail "no data.tar payload found in $ipk"
  tar -xf "$data_tar" -C "$cleanup/root"
  local appinfo_path
  appinfo_path="$(find "$cleanup/root/usr/palm/applications" -maxdepth 2 -type f -name appinfo.json -print -quit 2>/dev/null || true)"
  [[ -n "$appinfo_path" ]] || appinfo_path="$(find "$cleanup/root" -type f -name appinfo.json -print -quit)"
  [[ -n "$appinfo_path" ]] || fail "missing appinfo.json in $ipk"
  verify_package_root "$(dirname "$appinfo_path")"
  rm -rf "$cleanup"
  trap - RETURN
}

case "$rust_profile" in
  debug)
    cargo_profile_args=()
    cargo_profile_dir="debug"
    ;;
  release)
    cargo_profile_args=(--release)
    cargo_profile_dir="release"
    ;;
  *)
    cargo_profile_args=(--profile "$rust_profile")
    cargo_profile_dir="$rust_profile"
    ;;
esac

rdp_ffi_lib="${RDP_FFI_LIB:-${NATIVE_RDP_FFI_LIB:-$repo_root/webrdp-min/target/$target/$cargo_profile_dir/libwebrdp_min.a}}"

if [[ "$skip_build" != "1" ]]; then
  toolchain="$(find_toolchain)"
  [[ -f "$toolchain" ]] || fail "webOS toolchain file not found: $toolchain (set WEBOS_TOOLCHAIN_FILE=/path/to/toolchainfile.cmake)"
  sdk="$(cd "$(dirname "$toolchain")/../.." && pwd)"

  if [[ ! -f "$repo_root/third_party/IronRDP/Cargo.toml" || ! -f "$repo_root/third_party/ss4s/CMakeLists.txt" ||
        ! -f "$repo_root/third_party/commons/CMakeLists.txt" || ! -f "$repo_root/third_party/lvgl/CMakeLists.txt" ||
        ! -f "$repo_root/third_party/miniaudio/miniaudio.c" || ! -f "$repo_root/third_party/miniaudio/miniaudio.h" ]]; then
    fail "native submodules are not initialized; run: git submodule update --init third_party/IronRDP third_party/ss4s third_party/commons third_party/lvgl third_party/miniaudio"
  fi

  rustup target add "$target"
  CC_armv7_unknown_linux_gnueabi="$sdk/bin/arm-webos-linux-gnueabi-gcc" \
    AR_armv7_unknown_linux_gnueabi="$sdk/bin/arm-webos-linux-gnueabi-ar" \
    CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABI_LINKER="$sdk/bin/arm-webos-linux-gnueabi-gcc" \
    cargo build --manifest-path "$repo_root/webrdp-min/Cargo.toml" --features native --target "$target" "${cargo_profile_args[@]}"
  [[ -f "$rdp_ffi_lib" ]] || fail "Rust staticlib was not produced: $rdp_ffi_lib"

  cmake \
    -S "$repo_root/native" \
    -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DHELLOLG_WITH_SS4S=ON \
    -DHELLOLG_WITH_SDL=ON \
    -DHELLOLG_WITH_PRECONNECT_UI=ON \
    -DHELLOLG_LINK_RDP_FFI=ON \
    -DRDP_FFI_LIB="$rdp_ffi_lib" \
    -DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG -g" \
    -DCMAKE_CXX_FLAGS_MINSIZEREL="-Os -DNDEBUG -g" \
    "$@"
  cmake --build "$build_dir"
elif [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
  fail "NATIVE_WEBOS_SKIP_BUILD=1 but build directory is not configured: $build_dir"
fi

rm -rf "$stage_dir"
cmake --install "$build_dir" --prefix "$stage_dir"
# Runtime settings live in an in-app dir shipped /tmp-style (01777 + sticky): the install
# tree is root-owned on the TV, so the app (own uid) creates a private euid-suffixed
# subdir inside — same trust model as the /tmp fallback, but on persistent storage.
mkdir -p "$stage_dir/settings"
chmod 1777 "$stage_dir/settings"
verify_package_root "$stage_dir"

command -v ares-package >/dev/null 2>&1 ||
  fail "ares-package not found. Load the webOS CLI, for example: source ~/.nvm/nvm.sh && nvm use 22"

mkdir -p "$dist_dir"
marker="$dist_dir/.gnomecast-package-start.$$"
: > "$marker"
trap 'rm -f "$marker"' EXIT
set +e
ares-package "$stage_dir" -o "$dist_dir"
ares_status=$?
set -e
latest_ipk="$(find "$dist_dir" -maxdepth 1 -type f -name '*.ipk' -newer "$marker" -printf '%T@ %p\n' | sort -n | tail -n 1 | cut -d' ' -f2-)"
rm -f "$marker"
trap - EXIT

if [[ $ares_status -ne 0 && -z "$latest_ipk" ]]; then
  fail "ares-package failed with status $ares_status and no new .ipk was found in $dist_dir"
fi
[[ -n "$latest_ipk" ]] || fail "ares-package completed but no new .ipk was found in $dist_dir"
if [[ $ares_status -ne 0 ]]; then
  echo "build-native-webos: warning: ares-package exited with status $ares_status after writing an .ipk; verifying output" >&2
fi

verify_ipk "$latest_ipk"
printf '%s\n' "$latest_ipk" > "$dist_dir/latest-ipk.txt"
echo "build-native-webos: wrote $latest_ipk"
