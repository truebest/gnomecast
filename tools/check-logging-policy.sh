#!/usr/bin/env bash
# Enforce the single-sink logging policy for production native and Rust code.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failures=0

count_matches() {
  local pattern="$1"
  local file="$2"
  local matches

  matches="$(grep -Eo "$pattern" "$file" || true)"
  if [[ -z "$matches" ]]; then
    echo 0
  else
    printf '%s\n' "$matches" | wc -l | tr -d ' '
  fi
}

report_matches() {
  local label="$1"
  local pattern="$2"
  shift 2

  local matches
  matches="$(grep -EnH "$pattern" "$@" || true)"
  if [[ -n "$matches" ]]; then
    echo "check-logging-policy: $label" >&2
    printf '%s\n' "$matches" >&2
    failures=$((failures + 1))
  fi
}

native_sources=()
while IFS= read -r -d '' file; do
  case "$file" in
    */category_log.c|*/fonts/*)
      continue
      ;;
  esac
  native_sources+=("$file")
done < <(find "$repo_root/native/src" -type f -name '*.c' -print0 | sort -z)

if [[ ${#native_sources[@]} -eq 0 ]]; then
  echo "check-logging-policy: no production native sources found" >&2
  exit 2
fi

for file in "${native_sources[@]}"; do
  define_count="$(count_matches '^[[:space:]]*clog_define[[:space:]]*\(' "$file")"
  call_count="$(count_matches '^[[:space:]]*clog(_limited)?[[:space:]]*\(' "$file")"
  relative="${file#"$repo_root/"}"

  if [[ "$define_count" -ne 1 ]]; then
    echo "check-logging-policy: $relative has $define_count clog_define calls (expected exactly 1)" >&2
    failures=$((failures + 1))
  fi
  if [[ "$call_count" -eq 0 ]]; then
    echo "check-logging-policy: $relative has no clog/clog_limited calls" >&2
    failures=$((failures + 1))
  fi
done

# category_log.c is the stderr sink itself. The application modules must not
# bypass it; main.c may still redirect/configure stderr with freopen/setvbuf.
report_matches \
  "native code writes directly to stderr; use clog" \
  '(^|[^[:alnum:]_])(fprintf|vfprintf|fputs|fputc|putc|fwrite)[[:space:]]*\([^;]*stderr([^[:alnum:]_]|$)|(^|[^[:alnum:]_])(dprintf|vdprintf|write)[[:space:]]*\([[:space:]]*STDERR_FILENO([^[:alnum:]_]|$)' \
  "${native_sources[@]}"
report_matches \
  "native code calls perror; use clog with strerror(errno)" \
  '(^|[^[:alnum:]_])perror[[:space:]]*\(' \
  "${native_sources[@]}"
report_matches \
  "native code calls SDL_Log directly; use clog" \
  '(^|[^[:alnum:]_])SDL_Log[A-Za-z0-9_]*[[:space:]]*\(' \
  "${native_sources[@]}"

rust_sources=()
while IFS= read -r -d '' file; do
  rust_sources+=("$file")
done < <(find "$repo_root/webrdp-min/src" -type f -name '*.rs' -print0 | sort -z)

if [[ ${#rust_sources[@]} -gt 0 ]]; then
  report_matches \
    "Rust code calls eprintln!; forward through the native log callback" \
    '(^|[^[:alnum:]_])eprintln![[:space:]]*\(' \
    "${rust_sources[@]}"
  report_matches \
    "Rust code installs a stderr tracing formatter; use the native clog bridge" \
    'tracing_subscriber[[:space:]]*::[[:space:]]*fmt[[:space:]]*\(|(^|[^[:alnum:]_])((std|tokio)[[:space:]]*::[[:space:]]*)?io[[:space:]]*::[[:space:]]*stderr[[:space:]]*\(' \
    "${rust_sources[@]}"
fi

if [[ "$failures" -ne 0 ]]; then
  echo "check-logging-policy: failed with $failures policy violation(s)" >&2
  exit 1
fi

echo "check-logging-policy: checked ${#native_sources[@]} native source files and ${#rust_sources[@]} Rust source files"
