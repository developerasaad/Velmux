#!/usr/bin/env bash
# Collect the Android release artifacts (APK, AAB, R8 mapping, native
# symbols) plus SHA-256 checksums into a staging directory.
#
# Contract:
#   scripts/android/package.sh <staging-dir>   (after build.sh --release)
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

[ $# -eq 1 ] || vdie "usage: scripts/android/package.sh <staging-dir>"
STAGING="$1"
mkdir -p "$STAGING"

OUTPUTS="$VELMUX_ROOT/$VELMUX_ANDROID_HOST_DIR/build/outputs"
[ -d "$OUTPUTS/apk/release" ] || vdie "no release APK outputs found; run scripts/android/build.sh --release first"

cp "$OUTPUTS"/apk/release/*.apk "$STAGING/" 2>/dev/null || vdie "no APK in $OUTPUTS/apk/release"
if compgen -G "$OUTPUTS/bundle/release/*.aab" > /dev/null; then
  cp "$OUTPUTS"/bundle/release/*.aab "$STAGING/"
else
  vwarn "no AAB found in $OUTPUTS/bundle/release (bundleRelease did not run?)"
fi
cp "$OUTPUTS"/mapping/release/mapping.txt "$STAGING/r8-mapping.txt" 2>/dev/null || true

# Unstripped native libraries for crash symbolication.
(
  cd "$STAGING"
  zip -qr velmux-android-native-symbols.zip \
    "$OUTPUTS/intermediates/merged_native_libs/release" \
    "$OUTPUTS/intermediates/stripped_native_libs/release" 2>/dev/null || true
)

# Checksums for everything collected so far (the caller may add more).
(
  cd "$STAGING"
  find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -exec sha256sum {} + \
    > SHA256SUMS-android.txt
)

vlog "packaged Android artifacts into $STAGING:"
ls -la "$STAGING" >&2
