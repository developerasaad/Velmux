# Velmux Android app native code.
#
# Builds the shared C++ engine (core + platform backend + bridge facade) and
# the JNI adapter that the Kotlin TurboModule talks to. The FFmpeg backend
# links in when its cross-compiled artifacts are provided via
# VELMUX_FFMPEG_ANDROID_PREFIX (Milestone 3 wiring); until then the engine
# runs with the Android backend only and reports capabilities honestly.
cmake_minimum_required(VERSION 3.22)
project(velmux_app)

# cpp -> main -> src -> app -> android -> mobile -> apps -> repository root.
get_filename_component(VELMUX_REPO_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../" ABSOLUTE)
if(NOT EXISTS "${VELMUX_REPO_ROOT}/core/include/velmux/core/engine.h")
  message(FATAL_ERROR "Velmux repository root not found at ${VELMUX_REPO_ROOT}")
endif()

set(VELMUX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VELMUX_EMBEDDED_SQLITE ON CACHE BOOL "" FORCE)

# Optional FFmpeg mobile artifacts (out/android/<ABI> prefix produced by
# tools/ffmpeg-builder/build-android.sh).
set(VELMUX_FFMPEG_ANDROID_PREFIX "" CACHE PATH
  "Per-ABI FFmpeg prefix for the Android build")

set(_velmux_ffmpeg_prefix_for_abi "")
if(VELMUX_FFMPEG_ANDROID_PREFIX)
  if(EXISTS "${VELMUX_FFMPEG_ANDROID_PREFIX}/${ANDROID_ABI}/lib/pkgconfig/libavcodec.pc")
    set(_velmux_ffmpeg_prefix_for_abi "${VELMUX_FFMPEG_ANDROID_PREFIX}/${ANDROID_ABI}")
  else()
    message(WARNING "VELMUX_FFMPEG_ANDROID_PREFIX given but no per-ABI pkg-config found; building without the FFmpeg backend")
  endif()
endif()
set(VELMUX_FFMPEG_PREFIX "${_velmux_ffmpeg_prefix_for_abi}" CACHE PATH "" FORCE)

add_subdirectory("${VELMUX_REPO_ROOT}" velmux-engine)

add_library(velmux_jni SHARED velmux_jni.cpp)
target_link_libraries(velmux_jni PRIVATE velmux_bridge)
find_library(log-lib log)
target_link_libraries(velmux_jni PRIVATE ${log-lib})
