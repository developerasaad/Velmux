# Velmux engine + JNI adapter for the Android app.
#
# Child of the RN appmodules project (see ../CMakeLists.txt for why the
# top level belongs to React Native's application integration). Builds:
#   - the shared C++ engine (core + platform backends + bridge facade)
#     from the repository root, and
#   - libvelmux_jni.so: the JNI adapter the Kotlin TurboModule binds to
#     (RegisterNatives; loaded via System.loadLibrary("velmux_jni")).
#
# FFmpeg is MANDATORY for the product app (spec: the pinned per-ABI
# artifacts from tools/ffmpeg-builder/build-android.sh). A build without
# the FFmpeg backend would ship honest-but-crippled capabilities and there
# is intentionally no such path: a missing/incomplete prefix is a fatal
# configuration error, not a warning.
#
# velmux -> cpp -> main -> src -> app -> android -> mobile -> apps -> root.
get_filename_component(VELMUX_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../../" ABSOLUTE)
if(NOT EXISTS "${VELMUX_REPO_ROOT}/core/include/velmux/core/engine.h")
  message(FATAL_ERROR "Velmux repository root not found at ${VELMUX_REPO_ROOT}")
endif()

set(VELMUX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VELMUX_EMBEDDED_SQLITE ON CACHE BOOL "" FORCE)

# Per-ABI FFmpeg prefix (out/android/<ABI> from build-android.sh), passed
# by scripts/android/build.sh as -PvelmuxFFmpegPrefix. Required.
set(VELMUX_FFMPEG_ANDROID_PREFIX "" CACHE PATH
  "Per-ABI FFmpeg prefix for the Android build")
if(NOT VELMUX_FFMPEG_ANDROID_PREFIX)
  message(FATAL_ERROR
    "VELMUX_FFMPEG_ANDROID_PREFIX is not set: the Velmux app requires the "
    "pinned FFmpeg artifacts for every ABI (resolve them with "
    "scripts/android/build.sh, which fetches/builds them first; there is "
    "no app build without the FFmpeg backend)")
endif()
if(NOT EXISTS
    "${VELMUX_FFMPEG_ANDROID_PREFIX}/${ANDROID_ABI}/lib/pkgconfig/libavcodec.pc")
  message(FATAL_ERROR
    "no pinned FFmpeg artifacts for ABI ${ANDROID_ABI} under "
    "${VELMUX_FFMPEG_ANDROID_PREFIX} (expected "
    "${VELMUX_FFMPEG_ANDROID_PREFIX}/${ANDROID_ABI}/lib/pkgconfig/libavcodec.pc); "
    "resolve them with scripts/android/build.sh before building the app")
endif()
set(VELMUX_FFMPEG_PREFIX
  "${VELMUX_FFMPEG_ANDROID_PREFIX}/${ANDROID_ABI}" CACHE PATH "" FORCE)

add_subdirectory("${VELMUX_REPO_ROOT}" velmux-engine)

add_library(velmux_jni SHARED velmux_jni.cpp)
target_link_libraries(velmux_jni PRIVATE velmux_bridge)
find_library(log-lib log)
target_link_libraries(velmux_jni PRIVATE ${log-lib})
