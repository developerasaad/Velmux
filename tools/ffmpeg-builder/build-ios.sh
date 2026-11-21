#!/usr/bin/env bash
# Build the feature-selected FFmpeg backend for iOS (Xcode cross-compile).
#
# Contract (matches .github/workflows/ffmpeg-mobile.yml):
#   tools/ffmpeg-builder/build-ios.sh
#   env: VELMUX_IOS_DEPLOYMENT_TARGET (default 15.0, D-004)
#        VELMUX_FFMPEG_JOBS=<parallel jobs> (default: sysctl -n hw.ncpu)
#        VELMUX_WITH_SVT (default 1; SVT-AV1 encoder on arm64 slices)
#
# Slices:
#   iphoneos/arm64          -> out/ios/iphoneos/{include,lib,...}
#   iphonesimulator/arm64   -> merged (lipo) into
#   iphonesimulator/x86_64     out/ios/iphonesimulator/{include,lib,...}
#
# The x86_64 simulator slice omits SVT-AV1 (its x86 asm requires nasm and
# the slice is development-only); AV1 decode stays available via dav1d and
# the manifests report the difference honestly.
#
# Idempotent per slice: exits 0 quickly when that slice's manifest exists.
# Output also aggregates out/ios/manifest.json.
set -euo pipefail

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

DEPLOYMENT_TARGET="${VELMUX_IOS_DEPLOYMENT_TARGET:-15.0}"
WITH_SVT="${VELMUX_WITH_SVT:-1}"

case "$(uname -s)" in
  Darwin) ;;
  *) die "build-ios.sh must run on macOS (Xcode toolchain required)" ;;
esac

require_tool curl
require_tool make
require_tool python3
require_tool pkg-config
require_tool xcrun
require_tool lipo
ensure_cmake_ninja
ensure_meson

FFMPEG_VERSION="$(json_field "$BUILDER_ROOT/versions.json" "ffmpeg.version")"

log "Xcode: $(xcodebuild -version 2>/dev/null | head -1 || echo 'unknown')"
log "deployment target: $DEPLOYMENT_TARGET"
log "SVT-AV1: $([ "$WITH_SVT" = "1" ] && echo enabled || echo disabled)"

# ---------------------------------------------------------------------------
# Slice settings
# ---------------------------------------------------------------------------
slice_settings() {
  # $1 = slice name: device-arm64, sim-arm64, sim-x86_64
  case "$1" in
    device-arm64)
      SLICE_SDK="iphoneos"
      SLICE_ARCH="arm64"
      SLICE_MIN_FLAG="-miphoneos-version-min=$DEPLOYMENT_TARGET"
      SLICE_VPX_TARGET="arm64-darwin-gcc"
      SLICE_AUTOCONF_HOST="aarch64-apple-darwin"
      SLICE_MESON_CPU_FAMILY="aarch64"
      SLICE_MESON_CPU="aarch64"
      SLICE_OUT="iphoneos"
      SLICE_TAG="ios-device-arm64"
      SLICE_WANT_SVT="$WITH_SVT"
      ;;
    sim-arm64)
      SLICE_SDK="iphonesimulator"
      SLICE_ARCH="arm64"
      SLICE_MIN_FLAG="-mios-simulator-version-min=$DEPLOYMENT_TARGET"
      SLICE_VPX_TARGET="arm64-darwin-gcc"
      SLICE_AUTOCONF_HOST="aarch64-apple-darwin"
      SLICE_MESON_CPU_FAMILY="aarch64"
      SLICE_MESON_CPU="aarch64"
      SLICE_OUT="iphonesimulator"
      SLICE_TAG="ios-sim-arm64"
      SLICE_WANT_SVT="$WITH_SVT"
      ;;
    sim-x86_64)
      SLICE_SDK="iphonesimulator"
      SLICE_ARCH="x86_64"
      SLICE_MIN_FLAG="-mios-simulator-version-min=$DEPLOYMENT_TARGET"
      SLICE_VPX_TARGET="x86_64-darwin18-gcc"
      SLICE_AUTOCONF_HOST="x86_64-apple-darwin"
      SLICE_MESON_CPU_FAMILY="x86_64"
      SLICE_MESON_CPU="x86_64"
      SLICE_OUT="iphonesimulator"
      SLICE_TAG="ios-sim-x86_64"
      # SVT-AV1 x86_64 requires nasm for its assembly; this development-only
      # slice ships without the AV1 *encoder* (decode via dav1d remains).
      SLICE_WANT_SVT=0
      ;;
    *)
      die "unknown slice: $1"
      ;;
  esac
  SLICE_PREFIX="$OUT_DIR/ios/slices/$SLICE_TAG"
}

# ---------------------------------------------------------------------------
# Xcode toolchain wrappers (xcrun selects SDK + sysroot; flags target the
# slice arch/min-version). Plain single-command wrappers so autoconf, cmake,
# meson and FFmpeg's configure all accept them as CC/AR/... verbatim.
# ---------------------------------------------------------------------------
make_wrappers() {
  # build_tmp comes from the calling build_one_slice (bash dynamic scope).
  local bin="$build_tmp/bin-$SLICE_TAG"
  rm -rf "$bin"
  mkdir -p "$bin"
  # Binutils-style wrappers: xcrun selects SDK + sysroot, no extra flags.
  for tool in ar ranlib strip nm ld; do
    cat > "$bin/$tool" <<EOF
#!/bin/sh
exec xcrun -sdk $SLICE_SDK $tool "\$@"
EOF
    chmod +x "$bin/$tool"
  done
  # Compiler wrappers additionally inject the slice arch/min-version flags.
  cat > "$bin/clang" <<EOF
#!/bin/sh
exec xcrun -sdk $SLICE_SDK clang -arch $SLICE_ARCH $SLICE_MIN_FLAG "\$@"
EOF
  cat > "$bin/clang++" <<EOF
#!/bin/sh
exec xcrun -sdk $SLICE_SDK clang++ -arch $SLICE_ARCH $SLICE_MIN_FLAG "\$@"
EOF
  chmod +x "$bin/clang" "$bin/clang++"
  ln -sf "$bin/clang" "$bin/cc"
  ln -sf "$bin/clang++" "$bin/cxx"
  export PATH="$bin:$PATH"
  CC="$bin/cc"
  CXX="$bin/cxx"
  AR="$bin/ar"
  RANLIB="$bin/ranlib"
  STRIP="$bin/strip"
  NM="$bin/nm"
  for tool in "$CC" "$CXX" "$AR" "$RANLIB" "$STRIP" "$NM"; do
    [ -x "$tool" ] || die "wrapper not created: $tool"
  done
  export CC CXX AR RANLIB STRIP NM
}

# Meson cross file for the current slice.
write_meson_cross() {
  cat <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
ranlib = '$RANLIB'
strip = '$STRIP'
nm = '$NM'
pkg-config = 'pkg-config'

[host_machine]
system = 'darwin'
cpu_family = '$SLICE_MESON_CPU_FAMILY'
cpu = '$SLICE_MESON_CPU'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF
}

build_one_slice() {
  local slice="$1"
  slice_settings "$slice"
  local manifest="$SLICE_PREFIX/share/velmux/ffmpeg-capabilities.json"
  if [ -f "$SLICE_PREFIX/lib/pkgconfig/libavcodec.pc" ] && [ -f "$manifest" ]; then
    log "[$slice] build up to date"
    return 0
  fi

  local build_tmp="$SRC_DIR/build-$SLICE_TAG"
  mkdir -p "$build_tmp" "$SLICE_PREFIX"
  make_wrappers
  log "[$slice] building (sdk=$SLICE_SDK arch=$SLICE_ARCH)"

  # --- zlib (cmake iOS) --------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libz.a" ]; then
    log "[$slice] building zlib"
    zlib_tb="$(fetch_tarball zlib)"
    zlib_src="$(extract_tarball "$zlib_tb")"
    zlib_build="$build_tmp/zlib"
    rm -rf "$zlib_build"
    cmake -G Ninja -S "$zlib_src" -B "$zlib_build" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT="$SLICE_SDK" \
      -DCMAKE_OSX_ARCHITECTURES="$SLICE_ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SLICE_PREFIX" \
      -DCMAKE_FIND_ROOT_PATH="$SLICE_PREFIX" >/dev/null
    cmake --build "$zlib_build" >/dev/null
    cmake --install "$zlib_build" >/dev/null
    log "[$slice] zlib installed"
  fi

  # --- x264 (autoconf, per-slice source tree) ------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libx264.a" ]; then
    log "[$slice] building x264"
    x264_tb="$(fetch_tarball x264)"
    x264_src="$(extract_tarball "$x264_tb" "$SLICE_TAG")"
    (cd "$x264_src" && ./configure \
       --prefix="$SLICE_PREFIX" --enable-static --enable-pic --disable-cli \
       --host="$SLICE_AUTOCONF_HOST" \
       --extra-cflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG" \
       --extra-ldflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] x264 installed"
  fi

  # --- lame (autoconf) -------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libmp3lame.a" ]; then
    log "[$slice] building lame"
    lame_tb="$(fetch_tarball lame)"
    lame_src="$(extract_tarball "$lame_tb" "$SLICE_TAG")"
    (cd "$lame_src" && ./configure \
       --prefix="$SLICE_PREFIX" --disable-shared --enable-static \
       --disable-dependency-tracking --disable-gtktest --disable-nasm \
       --host="$SLICE_AUTOCONF_HOST" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] lame installed"
  fi

  # --- opus (autoconf) ----------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libopus.a" ]; then
    log "[$slice] building opus"
    opus_tb="$(fetch_tarball opus)"
    opus_src="$(extract_tarball "$opus_tb" "$SLICE_TAG")"
    (cd "$opus_src" && ./configure \
       --prefix="$SLICE_PREFIX" --disable-shared --enable-static \
       --disable-doc --disable-extra-programs \
       --host="$SLICE_AUTOCONF_HOST" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] opus installed"
  fi

  # --- libogg + libvorbis (autoconf) -----------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libogg.a" ]; then
    log "[$slice] building libogg"
    ogg_tb="$(fetch_tarball libogg)"
    ogg_src="$(extract_tarball "$ogg_tb" "$SLICE_TAG")"
    (cd "$ogg_src" && ./configure \
       --prefix="$SLICE_PREFIX" --disable-shared --enable-static \
       --host="$SLICE_AUTOCONF_HOST" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] libogg installed"
  fi
  if [ ! -f "$SLICE_PREFIX/lib/libvorbis.a" ]; then
    log "[$slice] building libvorbis"
    vorbis_tb="$(fetch_tarball libvorbis)"
    vorbis_src="$(extract_tarball "$vorbis_tb" "$SLICE_TAG")"
    (cd "$vorbis_src" && PKG_CONFIG_PATH="$SLICE_PREFIX/lib/pkgconfig" ./configure \
       --prefix="$SLICE_PREFIX" --disable-shared --enable-static --disable-docs \
       --host="$SLICE_AUTOCONF_HOST" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] libvorbis installed"
  fi

  # --- libvpx (custom configure) ------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libvpx.a" ]; then
    log "[$slice] building libvpx"
    vpx_tb="$(fetch_tarball libvpx)"
    vpx_src="$(extract_tarball "$vpx_tb" "$SLICE_TAG")"
    (cd "$vpx_src" && ./configure \
       --target="$SLICE_VPX_TARGET" \
       --prefix="$SLICE_PREFIX" \
       --disable-shared --enable-static --enable-pic \
       --disable-unit-tests --disable-examples --disable-tools --disable-docs \
       --disable-install-docs --disable-webm-io \
       --extra-cflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] libvpx installed"
  fi

  # --- dav1d (meson cross) --------------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libdav1d.a" ]; then
    log "[$slice] building dav1d"
    dav1d_tb="$(fetch_tarball dav1d)"
    dav1d_src="$(extract_tarball "$dav1d_tb")"
    dav1d_build="$build_tmp/dav1d"
    rm -rf "$dav1d_build"
    local cross_file="$build_tmp/dav1d-cross.meson"
    write_meson_cross > "$cross_file"
    env -u CC -u CXX meson setup "$dav1d_build" "$dav1d_src" \
      --prefix="$SLICE_PREFIX" --libdir=lib \
      --default-library=static --buildtype=release \
      --cross-file "$cross_file" \
      -Denable_tools=false -Denable_tests=false >/dev/null
    ninja -C "$dav1d_build" >/dev/null
    meson install -C "$dav1d_build" >/dev/null
    log "[$slice] dav1d installed"
  fi

  # --- SVT-AV1 (cmake iOS; arm64 slices) --------------------------------------------------------
  if [ "$SLICE_WANT_SVT" = "1" ]; then
    if [ ! -f "$SLICE_PREFIX/lib/libSvtAv1Enc.a" ]; then
      log "[$slice] building SVT-AV1"
      svt_tb="$(fetch_tarball svt-av1)"
      svt_src="$(extract_tarball "$svt_tb")"
      svt_build="$build_tmp/svt-av1"
      rm -rf "$svt_build"
      cmake -G Ninja -S "$svt_src" -B "$svt_build" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$SLICE_SDK" \
        -DCMAKE_OSX_ARCHITECTURES="$SLICE_ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SLICE_PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$SLICE_PREFIX" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
      cmake --build "$svt_build" >/dev/null
      cmake --install "$svt_build" >/dev/null
      log "[$slice] SVT-AV1 installed"
    fi
  else
    log "[$slice] SVT-AV1 skipped for this slice"
  fi

  # --- freetype (cmake iOS) ------------------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libfreetype.a" ]; then
    log "[$slice] building freetype"
    ft_tb="$(fetch_tarball freetype)"
    ft_src="$(extract_tarball "$ft_tb")"
    ft_build="$build_tmp/freetype"
    rm -rf "$ft_build"
    cmake -G Ninja -S "$ft_src" -B "$ft_build" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT="$SLICE_SDK" \
      -DCMAKE_OSX_ARCHITECTURES="$SLICE_ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SLICE_PREFIX" \
      -DCMAKE_FIND_ROOT_PATH="$SLICE_PREFIX" \
      -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=FALSE \
      -DFT_REQUIRE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE \
      -DFT_DISABLE_HARFBUZZ=TRUE >/dev/null
    cmake --build "$ft_build" >/dev/null
    cmake --install "$ft_build" >/dev/null
    log "[$slice] freetype installed"
  fi

  # --- libpng (cmake iOS) ------------------------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libpng16.a" ]; then
    log "[$slice] building libpng"
    png_tb="$(fetch_tarball libpng)"
    png_src="$(extract_tarball "$png_tb")"
    png_build="$build_tmp/libpng"
    rm -rf "$png_build"
    cmake -G Ninja -S "$png_src" -B "$png_build" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT="$SLICE_SDK" \
      -DCMAKE_OSX_ARCHITECTURES="$SLICE_ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SLICE_PREFIX" \
      -DCMAKE_FIND_ROOT_PATH="$SLICE_PREFIX" \
      -DBUILD_SHARED_LIBS=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
      -DPNG_SHARED=OFF -DPNG_EXECUTABLES=OFF >/dev/null
    cmake --build "$png_build" >/dev/null
    cmake --install "$png_build" >/dev/null
    log "[$slice] libpng installed"
  fi

  # --- harfbuzz (cmake iOS, with freetype) ----------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libharfbuzz.a" ]; then
    log "[$slice] building harfbuzz"
    hb_tb="$(fetch_tarball harfbuzz)"
    hb_src="$(extract_tarball "$hb_tb")"
    hb_build="$build_tmp/harfbuzz"
    rm -rf "$hb_build"
    cmake -G Ninja -S "$hb_src" -B "$hb_build" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT="$SLICE_SDK" \
      -DCMAKE_OSX_ARCHITECTURES="$SLICE_ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SLICE_PREFIX" \
      -DCMAKE_FIND_ROOT_PATH="$SLICE_PREFIX" \
      -DBUILD_SHARED_LIBS=OFF -DHB_BUILD_UTILS=OFF -DHB_BUILD_TESTS=OFF \
      -DHB_HAVE_GLIB=OFF -DHB_HAVE_FREETYPE=ON >/dev/null
    cmake --build "$hb_build" >/dev/null
    cmake --install "$hb_build" >/dev/null
    log "[$slice] harfbuzz installed"
  fi

  # --- fribidi (meson cross) ----------------------------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libfribidi.a" ]; then
    log "[$slice] building fribidi"
    fribidi_tb="$(fetch_tarball fribidi)"
    fribidi_src="$(extract_tarball "$fribidi_tb")"
    fribidi_build="$build_tmp/fribidi"
    rm -rf "$fribidi_build"
    local fribidi_cross="$build_tmp/fribidi-cross.meson"
    write_meson_cross > "$fribidi_cross"
    env -u CC -u CXX meson setup "$fribidi_build" "$fribidi_src" \
      --prefix="$SLICE_PREFIX" --libdir=lib \
      --default-library=static --buildtype=release \
      --cross-file "$fribidi_cross" \
      -Ddocs=false -Dtests=false >/dev/null
    ninja -C "$fribidi_build" >/dev/null
    meson install -C "$fribidi_build" >/dev/null
    log "[$slice] fribidi installed"
  fi

  # --- libass (autoconf) ----------------------------------------------------------------------------------------
  if [ ! -f "$SLICE_PREFIX/lib/libass.a" ]; then
    log "[$slice] building libass"
    ass_tb="$(fetch_tarball libass)"
    ass_src="$(extract_tarball "$ass_tb" "$SLICE_TAG")"
    (cd "$ass_src" && \
     PKG_CONFIG_PATH="$SLICE_PREFIX/lib/pkgconfig" \
     PKG_CONFIG_LIBDIR="$SLICE_PREFIX/lib/pkgconfig" \
     ./configure \
       --prefix="$SLICE_PREFIX" --disable-shared --enable-static \
       --disable-fontconfig --disable-require-system-font-provider \
       --host="$SLICE_AUTOCONF_HOST" \
       --extra-cflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG" \
       --extra-ldflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG" \
       >/dev/null && \
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] libass installed"
  fi

  # --- FFmpeg (out of tree) -----------------------------------------------------------------------------------------
  ffmpeg_tb="$(fetch_ffmpeg)"
  ffmpeg_src="$(extract_tarball "$ffmpeg_tb")"
  local ffbuild="$build_tmp/ffmpeg"
  if [ ! -f "$SLICE_PREFIX/lib/libavcodec.a" ]; then
    log "[$slice] configuring FFmpeg"
    rm -rf "$ffbuild"
    mkdir -p "$ffbuild"
    local svt_flag=""
    if [ "$SLICE_WANT_SVT" = "1" ]; then
      svt_flag="--enable-libsvtav1"
    fi
    (cd "$ffbuild" && "$ffmpeg_src/configure" \
       --prefix="$SLICE_PREFIX" \
       --pkg-config-flags=--static \
       --enable-cross-compile \
       --target-os=darwin \
       --arch="$SLICE_ARCH" \
       --cc="$CC" --cxx="$CXX" \
       --ar="$AR" --ranlib="$RANLIB" --nm="$NM" --strip="$STRIP" \
       --extra-cflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG -I$SLICE_PREFIX/include" \
       --extra-ldflags="-arch $SLICE_ARCH $SLICE_MIN_FLAG -L$SLICE_PREFIX/lib" \
       --disable-shared --enable-static --enable-pic \
       --disable-debug --disable-doc \
       --disable-ffplay \
       --disable-network \
       --disable-avdevice \
       --disable-autodetect \
       --disable-x86asm \
       --enable-pthreads \
       --enable-gpl --enable-libx264 \
       --enable-libmp3lame \
       --enable-libopus \
       --enable-libvorbis \
       --enable-libvpx \
       --enable-libdav1d \
       $svt_flag \
       --enable-libfreetype \
       --enable-libharfbuzz \
       --enable-libfribidi \
       --enable-libass \
       --enable-zlib \
       --enable-protocol=file \
       --enable-protocol=pipe \
       >/dev/null)
    log "[$slice] building FFmpeg with $JOBS jobs"
    (cd "$ffbuild" && make -j"$JOBS" >/dev/null && make install >/dev/null)
    log "[$slice] FFmpeg installed to $SLICE_PREFIX"
  fi

  # --- Capability manifest from the actual build tree -------------------------------------------------------------
  if [ ! -f "$manifest" ] || [ "$BUILDER_ROOT/manifest-from-config.py" -nt "$manifest" ]; then
    log "[$slice] generating capability manifest"
    python3 "$BUILDER_ROOT/manifest-from-config.py" \
      "$ffbuild" "$ffmpeg_src" "$FFMPEG_VERSION" "$manifest"
  fi

  # --- Validate against the Velmux feature requirements ------------------------------------------------------------
  local exempt=""
  if [ "$SLICE_WANT_SVT" = "0" ]; then
    exempt="--exempt libsvtav1"
  fi
  log "[$slice] validating build"
  python3 "$BUILDER_ROOT/validate.py" $exempt "$manifest"

  unset CC CXX AR RANLIB STRIP NM
  log "[$slice] build complete: $SLICE_PREFIX"
}

# ---------------------------------------------------------------------------
# Build every slice, then assemble the publishable layouts.
# ---------------------------------------------------------------------------
build_one_slice device-arm64
build_one_slice sim-arm64
build_one_slice sim-x86_64

log "assembling iphoneos layout"
DEVICE_OUT="$OUT_DIR/ios/iphoneos"
rm -rf "$DEVICE_OUT"
mkdir -p "$DEVICE_OUT"
cp -R "$OUT_DIR/ios/slices/ios-device-arm64/include" "$DEVICE_OUT/"
cp -R "$OUT_DIR/ios/slices/ios-device-arm64/lib" "$DEVICE_OUT/"
mkdir -p "$DEVICE_OUT/share/velmux"
cp "$OUT_DIR/ios/slices/ios-device-arm64/share/velmux/ffmpeg-capabilities.json" \
   "$DEVICE_OUT/share/velmux/"

log "assembling iphonesimulator layout (lipo arm64 + x86_64)"
SIM_OUT="$OUT_DIR/ios/iphonesimulator"
rm -rf "$SIM_OUT"
mkdir -p "$SIM_OUT/include" "$SIM_OUT/lib" "$SIM_OUT/share/velmux"
cp -R "$OUT_DIR/ios/slices/ios-sim-arm64/include/." "$SIM_OUT/include/"
cp "$OUT_DIR/ios/slices/ios-sim-arm64/share/velmux/ffmpeg-capabilities.json" \
   "$SIM_OUT/share/velmux/"
SIM_ARM64="$OUT_DIR/ios/slices/ios-sim-arm64"
SIM_X86="$OUT_DIR/ios/slices/ios-sim-x86_64"
for lib in "$SIM_ARM64"/lib/*.a; do
  name="$(basename "$lib")"
  if [ -f "$SIM_X86/lib/$name" ]; then
    lipo -create "$lib" "$SIM_X86/lib/$name" -output "$SIM_OUT/lib/$name"
  else
    # Slice-specific library (e.g. SVT-AV1 on arm64 only): ship as-is.
    cp "$lib" "$SIM_OUT/lib/$name"
  fi
done
# pkg-config files are consumed at build time only; ship the arm64 ones.
cp -R "$SIM_ARM64/lib/pkgconfig" "$SIM_OUT/lib/" 2>/dev/null || true

AGGREGATE="$OUT_DIR/ios/manifest.json"
log "aggregating manifests"
python3 - "$OUT_DIR/ios" "$FFMPEG_VERSION" "$DEPLOYMENT_TARGET" <<'PY'
import json, sys
from pathlib import Path

out_dir = Path(sys.argv[1])
version, target = sys.argv[2], sys.argv[3]

def load(rel):
    return json.loads(
        (out_dir / rel / "share" / "velmux" / "ffmpeg-capabilities.json").read_text())

device = load("iphoneos")
sim = load("iphonesimulator")
result = {
    "schema": 1,
    "target": "ios",
    "ffmpeg": version,
    "deploymentTarget": target,
    "iphoneos": {
        "archs": ["arm64"],
        "manifest": "iphoneos/share/velmux/ffmpeg-capabilities.json",
        "decoders": len(device["decoders"]),
        "encoders": len(device["encoders"]),
        "av1Encode": "libsvtav1" in {e["name"] for e in device["encoders"]},
        "configuration": device["configuration"],
    },
    "iphonesimulator": {
        "archs": ["arm64", "x86_64"],
        "manifest": "iphonesimulator/share/velmux/ffmpeg-capabilities.json",
        "decoders": len(sim["decoders"]),
        "encoders": len(sim["encoders"]),
        "av1Encode": "libsvtav1" in {e["name"] for e in sim["encoders"]},
        "configuration": sim["configuration"],
    },
}
(out_dir / "manifest.json").write_text(json.dumps(result, indent=1) + "\n")
print("aggregated iphoneos + iphonesimulator (fat arm64/x86_64)")
PY

log "ios build complete: $OUT_DIR/ios"
