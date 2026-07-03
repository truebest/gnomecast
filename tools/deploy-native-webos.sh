#!/usr/bin/env bash
set -euo pipefail
# Avoid leaking generated launch parameters if someone invokes this helper with bash -x.
set +x

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist_dir="${NATIVE_WEBOS_DIST_DIR:-$repo_root/dist/native-webos}"
appinfo="${NATIVE_WEBOS_APPINFO:-$repo_root/native/deploy/webos/appinfo.json}"
app_id="${NATIVE_WEBOS_APP_ID:-}"
config_path="${HELLOLG_NATIVE_CONFIG:-$repo_root/native/config.local.json}"
launch_with_defaults="${HELLOLG_LAUNCH_WITH_DEFAULTS:-0}"
ipk="${NATIVE_WEBOS_IPK:-}"
install_app=1
launch_app=1

fail() {
  echo "deploy-native-webos: $*" >&2
  exit 2
}

usage() {
  printf '%s\n' \
    "Usage: ARES_DEVICE=<tv-device> $0 [--ipk PATH] [--config PATH] [--with-defaults] [--no-install] [--no-launch]" \
    "" \
    "Installs the native webOS IPK and launches it with redacted RDP config params." \
    "If no IPK exists, this script runs tools/build-native-webos.sh first."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ipk)
      [[ $# -ge 2 ]] || fail "--ipk requires a path"
      ipk="$2"
      shift 2
      ;;
    --ipk=*)
      ipk="${1#--ipk=}"
      shift
      ;;
    --config)
      [[ $# -ge 2 ]] || fail "--config requires a path"
      config_path="$2"
      shift 2
      ;;
    --config=*)
      config_path="${1#--config=}"
      shift
      ;;
    --with-defaults)
      launch_with_defaults=1
      shift
      ;;
    --no-install)
      install_app=0
      shift
      ;;
    --no-launch)
      launch_app=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

if [[ "$install_app" == "1" ]]; then
  if [[ -z "$ipk" ]]; then
    if [[ ! -d "$dist_dir" || -z "$(find "$dist_dir" -maxdepth 1 -type f -name '*.ipk' -print -quit)" ]]; then
      "$repo_root/tools/build-native-webos.sh"
    fi
    ipk="$(find "$dist_dir" -maxdepth 1 -type f -name '*.ipk' -printf '%T@ %p\n' | sort -n | tail -n 1 | cut -d' ' -f2-)"
  fi
  [[ -f "$ipk" ]] || fail "package not found: $ipk"
fi

if [[ -z "$app_id" ]]; then
  [[ -f "$appinfo" ]] || fail "appinfo.json not found: $appinfo"
  app_id="$(python3 - "$appinfo" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as f:
    print(json.load(f)["id"])
PY
)"
fi
[[ -n "$app_id" ]] || fail "empty app id"

args=()
if [[ -n "${ARES_DEVICE:-}" ]]; then
  args=(-d "$ARES_DEVICE")
fi

if [[ "$install_app" == "1" ]]; then
  command -v ares-install >/dev/null 2>&1 ||
    fail "ares-install not found. Load the webOS CLI, for example: source ~/.nvm/nvm.sh && nvm use 22"
  echo "deploy-native-webos: installing $ipk" >&2
  ares-install "${args[@]}" "$ipk"
fi

if [[ "$launch_app" != "1" ]]; then
  exit 0
fi

command -v ares-launch >/dev/null 2>&1 ||
  fail "ares-launch not found. Load the webOS CLI, for example: source ~/.nvm/nvm.sh && nvm use 22"

if [[ "$launch_with_defaults" == "1" ]]; then
  echo "deploy-native-webos: launching $app_id with native defaults" >&2
  ares-launch "${args[@]}" "$app_id" --params '{"ignoreSavedConfig":true}'
  exit 0
fi

[[ -n "$config_path" ]] || fail "empty config path; set HELLOLG_NATIVE_CONFIG or use --config"
[[ -f "$config_path" ]] ||
  fail "config file not found: $config_path (not packaged). Use --with-defaults only for explicit defaults-only smoke launch."

launch_params="$(python3 - "$config_path" <<'PY'
import json
import sys

path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as f:
        config = json.load(f)
except Exception as exc:
    raise SystemExit(f"failed to read JSON config: {exc}")

if not isinstance(config, dict):
    raise SystemExit("config root must be a JSON object")

params = {}
for key in ("host", "username", "password", "domain"):
    if key not in config:
        continue
    value = config[key]
    if not isinstance(value, str):
        raise SystemExit(f"config field {key} must be a string")
    params[key] = value

for key, minimum, maximum in (
    ("port", 1, 65535),
    ("fps", 1, 240),
    ("wheelStep", 1, 120),
    ("wheelScrollDivisor", 1, 120),
):
    if key not in config:
        continue
    value = config[key]
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        raise SystemExit(f"config field {key} must be an integer in [{minimum}, {maximum}]")
    params[key] = value

if "relativeMouse" in config:
    value = config["relativeMouse"]
    if not isinstance(value, bool):
        raise SystemExit("config field relativeMouse must be a boolean")
    params["relativeMouse"] = value

print(json.dumps(params, separators=(",", ":")))
PY
)" || fail "failed to prepare launch parameters from $config_path"

echo "deploy-native-webos: launching $app_id with config from $config_path (credentials redacted)" >&2
ares-launch "${args[@]}" "$app_id" --params "$launch_params"
