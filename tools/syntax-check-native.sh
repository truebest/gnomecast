#!/usr/bin/env bash
# Fast stub-mode syntax check of the native C sources — the single source of
# truth for the file list that CI (bitbucket-pipelines.yml) and the docs
# (AGENTS.md, docs/build-environment.md, docs/native-runbook.md) used to
# hardcode and let drift. Every native/src/*.c, NDL adapter, and standalone
# backend source is checked automatically;
# only files whose HEADERS exist solely in the cross toolchain are skipped.
#
# Extra flags pass through, and CC can be overridden — e.g. a fast SDL-code
# check with the cross compiler:
#   CC=arm-webos-linux-gnueabi-gcc ./tools/syntax-check-native.sh -DHELLOLG_WITH_SDL=1 ...
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$repo_root/tools/check-logging-policy.sh"

# Files that cannot be syntax-checked on a bare host:
#   input_evdev.c   — includes libevdev headers (static lib in the webOS sysroot)
#   ui_preconnect.c — includes LVGL headers (configured only in the cross build)
excluded=(input_evdev.c ui_preconnect.c)

files=()
for f in "$repo_root"/native/src/*.c "$repo_root"/native/src/ndl_adapter/*.c \
         "$repo_root"/backend_ndl/src/backend_ndl.c; do
  base="$(basename "$f")"
  for skip in "${excluded[@]}"; do
    if [[ "$base" == "$skip" ]]; then
      continue 2
    fi
  done
  files+=("$f")
done

if [[ ${#files[@]} -eq 0 ]]; then
  echo "syntax-check-native: no sources matched native/src/*.c" >&2
  exit 2
fi

"${CC:-cc}" -fsyntax-only \
  -I"$repo_root/native/include" \
  -I"$repo_root/native/src/ndl_adapter" \
  -I"$repo_root/backend_ndl/include" \
  -I"$repo_root/backend_ndl/src" \
  -I"$repo_root/third_party/miniaudio" \
  "${files[@]}" "$@"

echo "syntax-check-native: checked ${#files[@]} files (skipped: ${excluded[*]})"
