#!/usr/bin/env bash
# Download a CI-built host FFmpeg artifact into the local tree.
#
# The canonical host FFmpeg builder is CI (core.yml uploads the pinned
# prefix as an identity-named artifact after the suite passes). This
# helper fetches it so local development runs ./scripts/test.sh against
# the exact pinned prefix instead of rebuilding it (~30 CPU-minutes).
#
# Usage:
#   scripts/ffmpeg/fetch-artifact.sh            # latest successful run on main
#   scripts/ffmpeg/fetch-artifact.sh <run-id>   # a specific run
#
# Environment:
#   GITHUB_TOKEN (or an origin remote URL embedding one) with actions:read
#   for the repository.
#
# The artifact's BUILD-IDENTITY is verified against the current tree
# before installation: a mismatched artifact is refused, never forced.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"
# shellcheck source=../../scripts/ffmpeg-builder/lib.sh
. "$VELMUX_ROOT/$VELMUX_FFMPEG_BUILDER_DIR/lib.sh"

REPO_API="https://api.github.com/repos/${VELMUX_GITHUB_REPO:-developerasaad/Velmux}"
TOKEN="${GITHUB_TOKEN:-}"
if [ -z "$TOKEN" ]; then
  # Fall back to a token embedded in the origin remote URL.
  remote="$(git -C "$VELMUX_ROOT" remote get-url origin 2>/dev/null || true)"
  case "$remote" in
    https://*@github.com/*)
      TOKEN="${remote#https://}"
      TOKEN="${TOKEN%%@*}"
      ;;
  esac
fi
[ -n "$TOKEN" ] || vdie "no GITHUB_TOKEN and no token in the origin remote"

auth=(-H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json")

RUN_ID="${1:-}"
if [ -z "$RUN_ID" ]; then
  vlog "finding the latest successful host-suite run on main"
  RUN_ID="$(curl -fsS "${auth[@]}" \
    "$REPO_API/actions/workflows/core.yml/runs?branch=main&status=success&per_page=1" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["workflow_runs"][0]["id"])')"
  [ -n "$RUN_ID" ] || vdie "no successful core.yml run found on main"
fi
vlog "run: $RUN_ID"

IDENTITY="$(compute_identity host)"
NAME="ffmpeg-host-$IDENTITY"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

vlog "downloading artifact $NAME"
ARTIFACT_URL="$(curl -fsS "${auth[@]}" \
  "$REPO_API/actions/runs/$RUN_ID/artifacts?per_page=100" |
  python3 -c "
import json,sys
name='$NAME'
for a in json.load(sys.stdin)['artifacts']:
    if a['name']==name and not a['expired']:
        print(a['archive_download_url']); break
")"
[ -n "$ARTIFACT_URL" ] \
  || vdie "artifact $NAME not found in run $RUN_ID (expired or not uploaded)"
curl -fsSL "${auth[@]}" -o "$TMP_DIR/a.zip" "$ARTIFACT_URL"

HOST_OUT="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/host"
vlog "installing into $HOST_OUT"
rm -rf "$HOST_OUT"
mkdir -p "$HOST_OUT"
unzip -q "$TMP_DIR/a.zip" -d "$HOST_OUT"
# The zip may carry a single top-level 'host' directory.
if [ -d "$HOST_OUT/host" ] && [ ! -f "$HOST_OUT/lib/pkgconfig/libavcodec.pc" ]; then
  mv "$HOST_OUT/host"/* "$HOST_OUT/" && rmdir "$HOST_OUT/host"
fi

# Identity gate: the artifact must match the current tree. A stale or
# foreign artifact is refused — reuse only when compatible.
identity_matches "$HOST_OUT" "$IDENTITY" \
  || vdie "downloaded artifact identity does not match the current tree (rebuild or pick another run)"

# Completeness gate (same library set as build-host.sh).
for lib in libx264.a libx265.a libmp3lame.a libopus.a libogg.a \
           libvorbis.a libvpx.a libdav1d.a libSvtAv1Enc.a libfreetype.a \
           libpng16.a libharfbuzz.a libfribidi.a libass.a libavcodec.a \
           libavformat.a libavutil.a libavfilter.a libswscale.a \
           libswresample.a; do
  [ -f "$HOST_OUT/lib/$lib" ] \
    || vdie "downloaded artifact is incomplete (missing $lib)"
done
[ -f "$HOST_OUT/lib/pkgconfig/libavcodec.pc" ] \
  || vdie "downloaded artifact is incomplete (no pkg-config files)"
[ -f "$HOST_OUT/share/velmux/ffmpeg-capabilities.json" ] \
  || vdie "downloaded artifact is incomplete (no capability manifest)"

fixup_pkgconfig_prefix "$HOST_OUT"
vlog "host FFmpeg artifact ready: $HOST_OUT (identity $IDENTITY)"
