#!/usr/bin/env bash
# Shared helpers for the Velmux build/test scripts (scripts/*).
# The FFmpeg builder has its own helper library
# (scripts/ffmpeg-builder/lib.sh); this one serves the top-level scripts.

# Shell strictness matching the builder: command substitution failures must
# propagate (inherit_errexit, guarded for bash < 4.4).
set -euo pipefail
if (shopt -s inherit_errexit) 2>/dev/null; then
  shopt -s inherit_errexit
fi

VELMUX_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VELMUX_ROOT="$(cd "$VELMUX_SCRIPTS_DIR/.." && pwd)"

# shellcheck source=lib/config.sh
. "$VELMUX_SCRIPTS_DIR/lib/config.sh"

vlog() { printf '[velmux] %s\n' "$*" >&2; }
vdie() { printf '[velmux] ERROR: %s\n' "$*" >&2; exit 1; }
vwarn() { printf '[velmux] WARNING: %s\n' "$*" >&2; }

require_tool() {
  command -v "$1" >/dev/null 2>&1 || vdie "required tool '$1' not found in PATH"
}

# Optional extra tool locations (e.g. a pip venv carrying cmake/ninja/meson
# when the system does not provide them).
if [ -n "${VELMUX_TOOL_PATH:-}" ]; then
  case ":$PATH:" in
    *":$VELMUX_TOOL_PATH:"*) ;;
    *) export PATH="$VELMUX_TOOL_PATH:$PATH" ;;
  esac
fi

# Absolute path of a repo-relative directory (dies when missing).
repo_dir() {
  local d="$VELMUX_ROOT/$1"
  [ -d "$d" ] || vdie "expected directory not found: $d"
  printf '%s\n' "$d"
}

# hash_file <file...> — stable multi-file hash (missing files contribute
# nothing but a marker, so adding a new input invalidates older identities).
hash_file() {
  local f
  for f in "$@"; do
    if [ -f "$VELMUX_ROOT/$f" ]; then
      printf '%s\0' "$f"
      cat "$VELMUX_ROOT/$f"
    else
      printf 'missing:%s\n' "$f"
    fi
  done | sha256sum | cut -d' ' -f1
}

# run_in <dir> <cmd...> — run a command in a directory (logs the cd).
run_in() {
  local dir="$1"; shift
  vlog "cd $dir && $*"
  (cd "$dir" && "$@")
}

# adb/emu helpers used by the device test scripts. Every adb call is
# timeout-wrapped: `adb shell` against an offline/half-booted device can
# block indefinitely and eat the whole job budget silently.
adb_t() {
  timeout "${VELMUX_ADB_TIMEOUT:-20}" adb "$@" 2>/dev/null || true
}

# resolve_android_ndk — locate the pinned NDK and export ANDROID_NDK_HOME.
# Order: existing ANDROID_NDK_HOME/ROOT (when the directory exists),
# $ANDROID_HOME/ndk/<pinned>, sdkmanager install as a fallback. The FFmpeg
# builder and the app's gradle build MUST use the same NDK revision
# (VELMUX_ANDROID_NDK_VERSION, kept in sync with gradle ext.ndkVersion).
resolve_android_ndk() {
  local ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
  if [ -n "$ndk" ] && [ -d "$ndk" ]; then
    export ANDROID_NDK_HOME="$ndk"
    vlog "using NDK: $ndk"
    return 0
  fi
  local sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
  if [ -z "$sdk" ] && [ -d /usr/local/lib/android/sdk ]; then
    sdk=/usr/local/lib/android/sdk
  fi
  if [ -n "$sdk" ] && [ -d "$sdk/ndk/$VELMUX_ANDROID_NDK_VERSION" ]; then
    export ANDROID_NDK_HOME="$sdk/ndk/$VELMUX_ANDROID_NDK_VERSION"
    vlog "using NDK: $ANDROID_NDK_HOME"
    return 0
  fi
  # Last resort: install the pinned NDK through sdkmanager when available.
  local sdkmanager=""
  if [ -n "$sdk" ]; then
    for c in "$sdk/cmdline-tools/latest/bin/sdkmanager" "$sdk/cmdline-tools/bin/sdkmanager"; do
      [ -x "$c" ] && sdkmanager="$c" && break
    done
    sdkmanager="${sdkmanager:-$(command -v sdkmanager 2>/dev/null || true)}"
  else
    sdkmanager="$(command -v sdkmanager 2>/dev/null || true)"
  fi
  if [ -n "$sdkmanager" ]; then
    vlog "installing NDK $VELMUX_ANDROID_NDK_VERSION through sdkmanager"
    yes | "$sdkmanager" "ndk;$VELMUX_ANDROID_NDK_VERSION" > /dev/null 2>&1 || true
    if [ -n "$sdk" ] && [ -d "$sdk/ndk/$VELMUX_ANDROID_NDK_VERSION" ]; then
      export ANDROID_NDK_HOME="$sdk/ndk/$VELMUX_ANDROID_NDK_VERSION"
      export ANDROID_SDK_ROOT="$sdk"
      export ANDROID_HOME="$sdk"
      vlog "using NDK: $ANDROID_NDK_HOME"
      return 0
    fi
  fi
  vdie "NDK $VELMUX_ANDROID_NDK_VERSION not found. Set ANDROID_NDK_HOME or ANDROID_HOME (with the NDK installed), or install it with: sdkmanager \"ndk;$VELMUX_ANDROID_NDK_VERSION\""
}
