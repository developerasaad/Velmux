# Velmux engine + JNI adapter for the Android app.
#
# Builds:
#   - the shared C++ engine (core + processing + platform backends + C ABI)
#     from the repository root, and
#   - libvelmux_jni.so: the JNI adapter the Kotlin engine client binds to
#     (RegisterNatives; loaded via System.loadLibrary("velmux_jni")).
#
# FFmpeg is MANDATORY for the product app (the pinned per-ABI artifacts
# from scripts/ffmpeg-builder/build-android.sh). A build without the FFmpeg
# processing module would ship honest-but-crippled capabilities and there is
# intentionally no such path: a missing/incomplete prefix is a fatal
# configuration error, not a warning.
#
# interop -> android -> platform -> root
get_filename_component(VELMUX_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../" ABSOLUTE)
if(NOT EXISTS "${VELMUX_REPO_ROOT}/core/include/velmux/engine.h")
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
    "no app build without the FFmpeg processing module)")
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
target_link_libraries(velmux_jni PRIVATE velmux::abi)
find_library(log-lib log)
target_link_libraries(velmux_jni PRIVATE ${log-lib})
