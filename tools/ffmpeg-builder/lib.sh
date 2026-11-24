#!/usr/bin/env bash
# Shared helpers for the Velmux FFmpeg builder.
# Targets: host (Linux/macOS), android (NDK), ios (Xcode).
#
# NOTE on error handling: callers capture function output via $( ) command
# substitution. Plain `set -e` does NOT propagate into command substitutions
# unless `inherit_errexit` is enabled — without it, failed builds silently
# continued and "installed" messages lied (caught in CI). Enabled below,
# guarded for bash < 4.4.
set -euo pipefail
if (shopt -s inherit_errexit) 2>/dev/null; then
  shopt -s inherit_errexit
fi

BUILDER_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOWNLOADS_DIR="${VELMUX_DOWNLOADS_DIR:-$BUILDER_ROOT/downloads}"
SRC_DIR="${VELMUX_SRC_DIR:-$BUILDER_ROOT/src}"
OUT_DIR="${VELMUX_OUT_DIR:-$BUILDER_ROOT/out}"

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

# sha256_of <file> — portable digest (GNU coreutils, macOS shasum, openssl).
# macOS Sequoia ships a BSD-style sha256sum without GNU's checksum-file mode,
# so verify by digest comparison instead of `sha256sum -c`.
sha256_of() {
  local file="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$file" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$file" | cut -d' ' -f1
  else
    openssl dgst -sha256 -r "$file" | cut -d' ' -f1
  fi
}

# verify_sha256 <expected> <file> — true when the file matches the pin.
verify_sha256() {
  [ "$(sha256_of "$2")" = "$1" ]
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

# ua_for_url <url> — print the User-Agent to use for a host, if any.
# Some hosts (code.videolan.org) reject plain curl/wget requests and need
# a browser-like UA; others (gitlab.com) run bot protection that REJECTS
# fake browser UAs while allowing the honest curl default. Nothing is
# printed for hosts that prefer the default.
ua_for_url() {
  case "$1" in
    https://code.videolan.org/* | http://code.videolan.org/*)
      printf '%s' "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"
      ;;
  esac
}

# download <name> — fetch + verify the pinned tarball recorded in versions.json.
# Tries the canonical URL first, then mirrors. Skips when the cached file
# already matches its checksum. The User-Agent is chosen per host
# (see ua_for_url).
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

  if [ -f "$dest" ] && verify_sha256 "$sha256" "$dest"; then
    log "using cached $fname"
    echo "$dest"
    return 0
  fi

  local urls=("$url")
  while IFS= read -r mirror; do
    [ -n "$mirror" ] && urls+=("$mirror")
  done < <(json_field "$BUILDER_ROOT/versions.json" "dependencies.$name.mirrors")

  local ok=0 ua
  for u in "${urls[@]}"; do
    log "downloading $name $version from $u"
    ua="$(ua_for_url "$u")"
    local ua_args=()
    [ -n "$ua" ] && ua_args=(-A "$ua")
    if curl -fsL "${ua_args[@]}" \
        --retry 3 --connect-timeout 30 \
        --speed-limit 1024 --speed-time 60 --max-time 900 \
        -o "$dest" "$u"; then ok=1; break; fi
    rm -f "$dest"
  done
  [ "$ok" = "1" ] || die "failed to download $name from any listed URL"

  verify_sha256 "$sha256" "$dest" || die "checksum mismatch for $fname (expected $sha256, got $(sha256_of "$dest"))"
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

  if [ -f "$dest" ] && verify_sha256 "$sha256" "$dest"; then
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
    # Stall protection: some mirrors accept the connection then stall the
    # body forever (nasm.us did exactly that from CI networks).
    if curl -fL --retry 3 --connect-timeout 30 \
        --speed-limit 1024 --speed-time 60 --max-time 900 \
        -o "$dest" "$u"; then ok=1; break; fi
    rm -f "$dest"
  done
  [ "$ok" = "1" ] || die "failed to download ffmpeg from any listed URL"

  verify_sha256 "$sha256" "$dest" || die "checksum mismatch for $fname (expected $sha256, got $(sha256_of "$dest"))"
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
  # First entry name; `head -1` closes the pipe early (harmless tar noise on
  # some versions), so the pipeline must not be errexit-fatal.
  top="$(tar -tf "$tarball" 2>/dev/null | head -1 | cut -d/ -f1 || true)"
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

# ---------------------------------------------------------------------------
# Build identity (artifact reuse contract)
#
# Every produced artifact records a BUILD-IDENTITY file: a hash over all
# inputs that determine its contents — the pinned versions (versions.json),
# the builder scripts themselves (configure flags live in them), the
# target/ABI/slice, the policy knobs (API level, deployment target, feature
# switches) and the toolchain revision that produced it. Consumers reuse an
# artifact ONLY when the recorded identity matches the identity computed
# from the current tree; anything else rebuilds. This prevents the
# "blindly consume the latest artifact" failure mode.
# ---------------------------------------------------------------------------

# _hash_input_line <label> <value-or-file>
_hash_input_line() {
  printf '%s=' "$1"
  if [ -f "$2" ]; then
    sha256_of "$2"
  else
    printf '%s' "$2"
  fi
  printf '\n'
}

# compute_identity <target> <extra-k=v...> — print the identity hash.
# Extra key=value pairs encode target policy knobs (api=29, deployment=15.0,
# svt=1, ...). Toolchain identity is appended automatically per target:
#   host  -> cc -dumpversion
#   android -> NDK Pkg.Revision from ANDROID_NDK_HOME/ROOT source.properties
#   ios   -> xcodebuild -version (first line)
compute_identity() {
  local target="$1"; shift
  local inputs=()
  local f
  for f in versions.json lib.sh "build-$target.sh"; do
    [ -f "$BUILDER_ROOT/$f" ] || die "identity input missing: $BUILDER_ROOT/$f"
    inputs+=("$(_hash_input_line "file:$f" "$BUILDER_ROOT/$f")")
  done
  inputs+=("target=$target")
  local kv
  for kv in "$@"; do
    inputs+=("knob:$kv")
  done
  case "$target" in
    host)
      inputs+=("toolchain=cc:$(cc -dumpversion 2>/dev/null || gcc -dumpversion 2>/dev/null || echo unknown)")
      ;;
    android)
      local ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
      local rev="unknown"
      if [ -n "$ndk" ] && [ -f "$ndk/source.properties" ]; then
        rev="$(sed -n 's/^Pkg.Revision *= *//p' "$ndk/source.properties" | tr -d '[:space:]')"
        [ -n "$rev" ] || rev="unreadable"
      fi
      inputs+=("toolchain=ndk:$rev")
      ;;
    ios)
      inputs+=("toolchain=xcode:$(xcodebuild -version 2>/dev/null | head -1 | tr -d '\n' || echo unknown)")
      ;;
  esac
  printf '%s\n' "${inputs[@]}" | sha256sum | cut -d' ' -f1
}

# identity_recorded <out-dir> — the hash recorded in BUILD-IDENTITY (empty
# when absent or unreadable).
identity_recorded() {
  local f="$1/BUILD-IDENTITY"
  if [ -f "$f" ]; then
    head -1 "$f" 2>/dev/null || true
  fi
}

# identity_matches <out-dir> <expected-hash>
identity_matches() {
  [ "$(identity_recorded "$1")" = "$2" ]
}

# write_identity <out-dir> <hash> <description> — records the identity plus
# a human-readable provenance block for debugging.
write_identity() {
  local dir="$1" hash="$2" desc="$3"
  mkdir -p "$dir"
  {
    printf '%s\n' "$hash"
    printf 'built: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || true)"
    printf '%s\n' "$desc"
  } > "$dir/BUILD-IDENTITY"
}

# fixup_pkgconfig_prefix <dir> — make restored artifacts position-independent:
# FFmpeg's generated .pc files bake the build-time prefix into absolute
# paths; rewrite prefix= to the directory the artifact actually lives in
# now. Everything else in the .pc files is ${prefix}-relative.
fixup_pkgconfig_prefix() {
  local dir="$1" pc
  [ -d "$dir" ] || return 0
  local abs
  abs="$(cd "$dir" && pwd)"
  for pc in "$dir"/lib/pkgconfig/*.pc "$dir"/lib64/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    sed -i.bak "s|^prefix=.*|prefix=$abs|" "$pc" && rm -f "$pc.bak"
  done
}
