#!/usr/bin/env bash
# Shared helpers for the Velmux FFmpeg builder.
# Targets: host (Linux/macOS), android (NDK), ios (Xcode).
set -euo pipefail

BUILDER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOWNLOADS_DIR="${VELMUX_DOWNLOADS_DIR:-$BUILDER_ROOT/downloads}"
SRC_DIR="$BUILDER_ROOT/src"
OUT_DIR="$BUILDER_ROOT/out"

JOBS="${VELMUX_FFMPEG_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

# NOTE: log() writes to stderr on purpose — functions that print captured
# values (fetch_tarball, extract_tarball, json_field) must keep stdout clean.
log() { printf '[velmux-ffmpeg] %s\n' "$*" >&2; }
die() { printf '[velmux-ffmpeg] ERROR: %s\n' "$*" >&2; exit 1; }

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "required tool '$1' not found in PATH"
}

# Optional extra tool locations (e.g. a user venv carrying cmake/ninja/meson
# when the system does not). Set VELMUX_TOOL_PATH to a colon-separated list.
if [ -n "${VELMUX_TOOL_PATH:-}" ]; then
  case ":$PATH:" in
    *":$VELMUX_TOOL_PATH:"*) ;;
    *) export PATH="$VELMUX_TOOL_PATH:$PATH" ;;
  esac
fi

# Ensure cmake and ninja are usable (dav1d, fribidi and friends build with
# them; FFmpeg's configure accepts ninja makefiles).
ensure_cmake_ninja() {
  command -v cmake >/dev/null 2>&1 || die "cmake not found"
  command -v ninja >/dev/null 2>&1 || die "ninja not found"
}

# ensure_meson: dav1d builds with meson.
ensure_meson() {
  command -v meson >/dev/null 2>&1 || die "meson not found (install with: pip install meson)"
}

# json_field <file> <dot.path> — minimal JSON reader using python3.
# Prints nothing when the path does not exist.
json_field() {
  python3 - "$1" "$2" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as fh:
        doc = json.load(fh)
    node = doc
    for part in sys.argv[2].split('.'):
        node = node[part]
except (KeyError, TypeError, json.JSONDecodeError, OSError):
    sys.exit(0)
if isinstance(node, list):
    for item in node:
        print(item)
else:
    print(node)
PY
}

# download <name> — fetch + verify the pinned tarball recorded in versions.json.
# Tries the canonical URL first, then mirrors. Skips when the cached file
# already matches its checksum. A browser-like User-Agent is used because some
# canonical hosts (code.videolan.org) reject plain curl/wget requests.
fetch_tarball() {
  local name="$1"
  local version url sha256 fname
  version="$(json_field "$BUILDER_ROOT/versions.json" "dependencies.$name.version")"
  url="$(json_field "$BUILDER_ROOT/versions.json" "dependencies.$name.url" | head -1)"
  sha256="$(json_field "$BUILDER_ROOT/versions.json" "dependencies.$name.sha256")"
  local override
  override="$(json_field "$BUILDER_ROOT/versions.json" "dependencies.$name.file" | head -1)"
  if [ -n "${override:-}" ] && [ "$override" != "None" ]; then
    fname="$override"
  else
    fname="$(basename "$url")"
  fi
  mkdir -p "$DOWNLOADS_DIR"
  local dest="$DOWNLOADS_DIR/$fname"

  if [ -f "$dest" ] && echo "$sha256  $dest" | sha256sum -c --status 2>/dev/null; then
    log "using cached $fname"
    echo "$dest"
    return 0
  fi

  local urls=("$url")
  while IFS= read -r mirror; do
    [ -n "$mirror" ] && urls+=("$mirror")
  done < <(json_field "$BUILDER_ROOT/versions.json" "dependencies.$name.mirrors")

  local ok=0
  for u in "${urls[@]}"; do
    log "downloading $name $version from $u"
    if curl -fsL -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36" \
        --retry 3 --connect-timeout 30 -o "$dest" "$u"; then ok=1; break; fi
    rm -f "$dest"
  done
  [ "$ok" = "1" ] || die "failed to download $name from any listed URL"

  echo "$sha256  $dest" | sha256sum -c --status || die "checksum mismatch for $fname (expected $sha256)"
  log "verified $fname"
  echo "$dest"
}

fetch_ffmpeg() {
  local version url sha256 fname
  version="$(json_field "$BUILDER_ROOT/versions.json" "ffmpeg.version")"
  url="$(json_field "$BUILDER_ROOT/versions.json" "ffmpeg.url" | head -1)"
  sha256="$(json_field "$BUILDER_ROOT/versions.json" "ffmpeg.sha256")"
  fname="$(basename "$url")"
  mkdir -p "$DOWNLOADS_DIR"
  local dest="$DOWNLOADS_DIR/$fname"

  if [ -f "$dest" ] && echo "$sha256  $dest" | sha256sum -c --status 2>/dev/null; then
    log "using cached $fname"
    echo "$dest"
    return 0
  fi

  local urls=("$url")
  while IFS= read -r mirror; do
    [ -n "$mirror" ] && urls+=("$mirror")
  done < <(json_field "$BUILDER_ROOT/versions.json" "ffmpeg.mirrors")

  local ok=0
  for u in "${urls[@]}"; do
    log "downloading ffmpeg $version from $u"
    if curl -fL --retry 3 --connect-timeout 30 -o "$dest" "$u"; then ok=1; break; fi
    rm -f "$dest"
  done
  [ "$ok" = "1" ] || die "failed to download ffmpeg from any listed URL"

  echo "$sha256  $dest" | sha256sum -c --status || die "checksum mismatch for $fname (expected $sha256)"
  log "verified $fname"
  echo "$dest"
}

# extract <tarball> [tag] — unpack into SRC_DIR (optionally under a per-target
# subdirectory "tag" so cross builds never reuse an in-tree configure run),
# print the resulting source directory. Handles tarballs whose top-level
# directory name differs from the file name.
extract_tarball() {
  local tarball="$1"
  local tag="${2:-}"
  mkdir -p "$SRC_DIR"
  local dest_root="$SRC_DIR"
  local stamp="$tarball.extracted"
  if [ -n "$tag" ]; then
    dest_root="$SRC_DIR/$tag"
    mkdir -p "$dest_root"
    stamp="$SRC_DIR/$tag/$(basename "$tarball").extracted"
  fi
  if [ -f "$stamp" ] && [ -d "$(cat "$stamp")" ]; then
    cat "$stamp"
    return 0
  fi
  local tmp_marker
  tmp_marker="$(mktemp "$SRC_DIR/.extractXXXXXX")"
  tar -xf "$tarball" -C "$dest_root"
  local top
  # First entry name; stderr silenced because `head -1` closes the pipe and
  # tar reports a harmless "write error" on some versions.
  top="$(tar -tf "$tarball" 2>/dev/null | head -1 | cut -d/ -f1)"
  local dir="$dest_root/$top"
  [ -d "$dir" ] || die "extraction of $tarball produced unexpected layout"
  echo "$dir" > "$stamp"
  rm -f "$tmp_marker"
  echo "$dir"
}

# with_pkg_config <prefix> <command...> — run a command with PKG_CONFIG_PATH
# pointing at <prefix>/lib/pkgconfig (and lib64 fallback).
with_pkg_config() {
  local prefix="$1"; shift
  PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" "$@"
}

# host_has_system <pkg-config-name> — true when the host provides a system lib.
host_has_system() {
  pkg-config --exists "$1" 2>/dev/null
}
