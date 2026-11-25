cmake_minimum_required(VERSION 3.21)
project(velmux LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VELMUX_BUILD_TESTS "Build test binaries (doctest)" ON)
# FFmpeg artifacts prefix produced by scripts/ffmpeg-builder/build-host.sh.
# When empty, the build looks for FFmpeg via pkg-config; when that also
# fails, the FFmpeg processing module is skipped with a warning (CI always
# provides the prefix, so the module and its integration tests always build
# there).
set(VELMUX_FFMPEG_PREFIX "" CACHE PATH "Prefix of the Velmux-built FFmpeg")
# Build the vendored SQLite amalgamation instead of finding a system SQLite.
# Required for targets whose toolchain does not expose SQLite to native code
# (Android NDK); the amalgamation is public domain (third_party/sqlite).
option(VELMUX_EMBEDDED_SQLITE "Build the vendored SQLite amalgamation" OFF)

# ---------------------------------------------------------------------------
# Core library — the authoritative application and media-processing core.
#
# Public surface: core/include/velmux (see velmux.h). Internal headers live
# under core/src/<module>/ and are shared only with the in-tree processing
# modules, platform backends and tests through the velmux::internal
# interface target.
# ---------------------------------------------------------------------------
add_library(velmux_core
  core/src/catalog/types.cpp
  core/src/catalog/features.cpp
  core/src/capabilities/capabilities.cpp
  core/src/compression/compression.cpp
  core/src/planning/planner.cpp
  core/src/jobs/events.cpp
  core/src/jobs/job_manager.cpp
  core/src/jobs/scheduler.cpp
  core/src/persistence/job_store.cpp
  core/src/persistence/telemetry.cpp
  core/src/recovery/recovery.cpp
  core/src/validation/validation.cpp
  core/src/processing/hybrid_executor.cpp
  core/src/api/engine.cpp
)
add_library(velmux::core ALIAS velmux_core)
target_include_directories(velmux_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>)
target_compile_features(velmux_core PUBLIC cxx_std_20)

# Internal headers (processing contracts, catalog, planning, jobs,
# persistence). Only for in-tree engine modules and tests — never part of
# the public API.
add_library(velmux_internal INTERFACE)
add_library(velmux::internal ALIAS velmux_internal)
target_include_directories(velmux_internal INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/src>)
target_link_libraries(velmux_core PRIVATE velmux::internal)

find_package(Threads REQUIRED)
target_link_libraries(velmux_core PUBLIC Threads::Threads)

if(VELMUX_EMBEDDED_SQLITE)
  add_library(velmux_sqlite STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
  target_include_directories(velmux_sqlite
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite>)
  # Amalgamation build: no dynamic SQLite features needed by the core.
  target_compile_definitions(velmux_sqlite PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_JOURNAL_MODE_LIMITED=1 SQLITE_ENABLE_FTS5=0)
  target_link_libraries(velmux_core PUBLIC velmux_sqlite)
  if(UNIX)
    target_compile_options(velmux_sqlite PRIVATE -w)
  endif()
else()
  find_package(SQLite3 REQUIRED)
  target_link_libraries(velmux_core PUBLIC SQLite::SQLite3)
endif()

if(UNIX)
  target_compile_options(velmux_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# FFmpeg processing module (platform-neutral libav* integration, owned by
# the core; direct library usage — no CLI subprocess architecture)
# ---------------------------------------------------------------------------
set(VELMUX_FFMPEG_FOUND FALSE)
set(VELMUX_FFMPEG_PC_PATH "")
if(VELMUX_FFMPEG_PREFIX)
  if(EXISTS "${VELMUX_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc")
    set(VELMUX_FFMPEG_PC_PATH "${VELMUX_FFMPEG_PREFIX}/lib/pkgconfig")
    set(VELMUX_FFMPEG_FOUND TRUE)
    message(STATUS "Velmux FFmpeg prefix: ${VELMUX_FFMPEG_PREFIX}")
  else()
    message(WARNING
      "VELMUX_FFMPEG_PREFIX given but no pkg-config files found there; "
      "FFmpeg processing module disabled")
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --exists libavcodec libavformat
      RESULT_VARIABLE _system_ffmpeg_ok)
    if(_system_ffmpeg_ok EQUAL 0)
      set(VELMUX_FFMPEG_FOUND TRUE)
      message(STATUS "FFmpeg found via system pkg-config")
    endif()
  endif()
endif()

if(VELMUX_FFMPEG_FOUND)
  # The queries below need the pkg-config executable. It is NOT guaranteed
  # to be discovered implicitly: find_package(SQLite3) happens to define
  # PKG_CONFIG_EXECUTABLE as a side effect on host builds, but embedded-
  # SQLite targets (Android NDK) never call it, which silently disabled the
  # module even with a valid prefix. Resolve it explicitly.
  find_package(PkgConfig REQUIRED)
  # Resolve flags once at configure time. Static linking of the Velmux-built
  # FFmpeg needs --static so the pinned external codec libs come along.
  set(_pc_env "PKG_CONFIG_PATH=${VELMUX_FFMPEG_PC_PATH}:$ENV{PKG_CONFIG_PATH}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env ${_pc_env}
            ${PKG_CONFIG_EXECUTABLE} --static --cflags
            libavcodec libavformat libavutil libavfilter libswscale libswresample
    OUTPUT_VARIABLE VELMUX_FFMPEG_CFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _cflags_ok)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env ${_pc_env}
            ${PKG_CONFIG_EXECUTABLE} --static --libs
            libavcodec libavformat libavutil libavfilter libswscale libswresample
    OUTPUT_VARIABLE VELMUX_FFMPEG_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _libs_ok)
  if(NOT _cflags_ok EQUAL 0 OR NOT _libs_ok EQUAL 0)
    message(WARNING "pkg-config query for FFmpeg failed; module disabled")
    set(VELMUX_FFMPEG_FOUND FALSE)
  else()
    add_library(velmux_ffmpeg
      core/src/processing/ffmpeg/codec_mapping.cpp
      core/src/processing/ffmpeg/ffmpeg_backend.cpp
      core/src/processing/ffmpeg/ffmpeg_stages.cpp
      core/src/processing/ffmpeg/subtitle_tools.cpp
    )
    add_library(velmux::ffmpeg ALIAS velmux_ffmpeg)
    target_link_libraries(velmux_ffmpeg
      PUBLIC velmux::core velmux::internal)
    separate_arguments(VELMUX_FFMPEG_CFLAGS_SPLIT NATIVE_COMMAND
                       "${VELMUX_FFMPEG_CFLAGS}")
    separate_arguments(VELMUX_FFMPEG_LIBS_SPLIT NATIVE_COMMAND
                       "${VELMUX_FFMPEG_LIBS}")
    target_compile_options(velmux_ffmpeg PRIVATE
                           ${VELMUX_FFMPEG_CFLAGS_SPLIT})
    # Raw -l/-L flags go through target_link_libraries so they appear AFTER
    # the objects (static archives resolve left-to-right).
    target_link_libraries(velmux_ffmpeg PUBLIC ${VELMUX_FFMPEG_LIBS_SPLIT})
    if(UNIX)
      target_compile_options(velmux_ffmpeg PRIVATE -Wall -Wextra)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Platform native backends (OS-specific media integration; physically owned
# by platform/<os>/native per the repository structure rules)
# ---------------------------------------------------------------------------
add_library(velmux_backend_android
  platform/android/native/android_backend.cpp
  platform/android/native/android_stages.cpp
  platform/android/native/media_transcoder.cpp)
add_library(velmux::backend-android ALIAS velmux_backend_android)
target_link_libraries(velmux_backend_android
  PUBLIC velmux::core velmux::internal ${CMAKE_DL_LIBS})
if(ANDROID)
  # NDK media APIs for the real MediaCodec pipeline (media_transcoder.cpp).
  target_link_libraries(velmux_backend_android PUBLIC mediandk android
                        nativewindow log)
endif()

add_library(velmux_backend_apple platform/ios/native/apple_backend.cpp)
add_library(velmux::backend-apple ALIAS velmux_backend_apple)
target_link_libraries(velmux_backend_apple
  PUBLIC velmux::core velmux::internal)
if(APPLE)
  target_sources(velmux_backend_apple PRIVATE
    platform/ios/native/apple_pipeline.mm
    platform/ios/native/apple_stages.mm)
  set_source_files_properties(platform/ios/native/apple_pipeline.mm
    PROPERTIES LANGUAGE OBJCXX)
  set_source_files_properties(platform/ios/native/apple_stages.mm
    PROPERTIES LANGUAGE OBJCXX)
  # AVFoundation/VideoToolbox/CoreMedia/CoreVideo for the native pipeline.
  target_link_libraries(velmux_backend_apple PUBLIC
    "-framework AVFoundation" "-framework VideoToolbox"
    "-framework CoreMedia" "-framework CoreVideo"
    "-framework Foundation" "-framework CoreFoundation")
endif()

# ---------------------------------------------------------------------------
# Public C ABI — the versioned application integration contract consumed by
# the Kotlin engine clients (JNI on Android, C interop on iOS).
# ---------------------------------------------------------------------------
add_library(velmux_abi core/src/api/abi.cpp)
add_library(velmux::abi ALIAS velmux_abi)
target_link_libraries(velmux_abi
  PUBLIC velmux::core velmux::backend-android velmux::backend-apple)
target_include_directories(velmux_abi PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/android/native
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/native)
target_compile_definitions(velmux_abi PRIVATE
  VELMUX_WITH_ANDROID_BACKEND=1 VELMUX_WITH_APPLE_BACKEND=1)
if(VELMUX_FFMPEG_FOUND)
  target_link_libraries(velmux_abi PUBLIC velmux::ffmpeg)
  target_compile_definitions(velmux_abi PRIVATE VELMUX_WITH_FFMPEG_BACKEND=1)
endif()
if(UNIX)
  target_compile_options(velmux_abi PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VELMUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
  if(VELMUX_FFMPEG_FOUND)
    add_subdirectory(core/tests/ffmpeg)
    add_subdirectory(core/tests/abi)
  else()
    message(STATUS "FFmpeg module not built; its integration tests are skipped")
  endif()
endif()
