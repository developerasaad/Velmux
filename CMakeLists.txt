cmake_minimum_required(VERSION 3.21)
project(velmux LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VELMUX_BUILD_TESTS "Build test binaries (doctest)" ON)
# FFmpeg backend prefix produced by tools/ffmpeg-builder/build-host.sh.
# When empty, the build looks for FFmpeg via pkg-config; when that also
# fails, the FFmpeg backend is skipped with a warning (CI always provides
# the prefix, so the backend and its integration tests always build there).
set(VELMUX_FFMPEG_PREFIX "" CACHE PATH "Prefix of the Velmux-built FFmpeg")
# Build the vendored SQLite amalgamation instead of finding a system SQLite.
# Required for targets whose toolchain does not expose SQLite to native code
# (Android NDK); the amalgamation is public domain (third_party/sqlite).
option(VELMUX_EMBEDDED_SQLITE "Build the vendored SQLite amalgamation" OFF)

# ---------------------------------------------------------------------------
# Core library (no React Native dependency; reusable from any host)
# ---------------------------------------------------------------------------
add_library(velmux_core
  core/src/types.cpp
  core/src/feature_registry.cpp
  core/src/capability_registry.cpp
  core/src/compression.cpp
  core/src/planner.cpp
  core/src/events.cpp
  core/src/job_store.cpp
  core/src/recovery.cpp
  core/src/telemetry.cpp
  core/src/validation.cpp
  core/src/scheduler.cpp
  core/src/job_manager.cpp
  core/src/engine.cpp
)
add_library(velmux::core ALIAS velmux_core)
target_include_directories(velmux_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/contracts/include>
)
target_compile_features(velmux_core PUBLIC cxx_std_20)

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
# FFmpeg backend (libav* libraries directly; no CLI subprocess architecture)
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
      "FFmpeg backend disabled")
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
    message(WARNING "pkg-config query for FFmpeg failed; backend disabled")
    set(VELMUX_FFMPEG_FOUND FALSE)
  else()
    add_library(velmux_backend_ffmpeg
      backends/ffmpeg/src/codec_mapping.cpp
      backends/ffmpeg/src/ffmpeg_backend.cpp
      backends/ffmpeg/src/subtitle_tools.cpp
    )
    add_library(velmux::backend-ffmpeg ALIAS velmux_backend_ffmpeg)
    target_include_directories(velmux_backend_ffmpeg
      PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/ffmpeg/include>)
    target_link_libraries(velmux_backend_ffmpeg PUBLIC velmux::core)
    separate_arguments(VELMUX_FFMPEG_CFLAGS_SPLIT NATIVE_COMMAND
                       "${VELMUX_FFMPEG_CFLAGS}")
    separate_arguments(VELMUX_FFMPEG_LIBS_SPLIT NATIVE_COMMAND
                       "${VELMUX_FFMPEG_LIBS}")
    target_compile_options(velmux_backend_ffmpeg PRIVATE
                           ${VELMUX_FFMPEG_CFLAGS_SPLIT})
    # Raw -l/-L flags go through target_link_libraries so they appear AFTER
    # the objects (static archives resolve left-to-right).
    target_link_libraries(velmux_backend_ffmpeg PUBLIC velmux::core
                          ${VELMUX_FFMPEG_LIBS_SPLIT})
    if(UNIX)
      target_compile_options(velmux_backend_ffmpeg PRIVATE -Wall -Wextra)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Platform backend skeletons (honest availability reporting)
# ---------------------------------------------------------------------------
add_library(velmux_backend_android
  backends/android/cpp/android_backend.cpp
  backends/android/cpp/media_transcoder.cpp)
target_link_libraries(velmux_backend_android PUBLIC velmux::core ${CMAKE_DL_LIBS})
target_include_directories(velmux_backend_android
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/android/cpp>)
if(ANDROID)
  # NDK media APIs for the real MediaCodec pipeline (media_transcoder.cpp).
  target_link_libraries(velmux_backend_android PUBLIC mediandk android
                        nativewindow log)
endif()

add_library(velmux_backend_apple backends/apple/objcpp/apple_backend.cpp)
target_link_libraries(velmux_backend_apple PUBLIC velmux::core)
target_include_directories(velmux_backend_apple
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backends/apple/objcpp>)

# ---------------------------------------------------------------------------
# Bridge (engine-side serialization + stable C facade for hosts)
# ---------------------------------------------------------------------------
add_library(velmux_bridge
  bridge/cpp/velmux_bridge.cpp
)
target_include_directories(velmux_bridge
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/bridge/cpp>)
target_link_libraries(velmux_bridge
  PUBLIC velmux::core velmux_backend_android velmux_backend_apple)
target_compile_definitions(velmux_bridge PRIVATE
  VELMUX_WITH_ANDROID_BACKEND=1 VELMUX_WITH_APPLE_BACKEND=1)
if(VELMUX_FFMPEG_FOUND)
  target_link_libraries(velmux_bridge PUBLIC velmux::backend-ffmpeg)
  target_compile_definitions(velmux_bridge PRIVATE VELMUX_WITH_FFMPEG_BACKEND=1)
endif()
if(UNIX)
  target_compile_options(velmux_bridge PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VELMUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
  if(VELMUX_FFMPEG_FOUND)
    add_subdirectory(backends/ffmpeg/tests)
    add_subdirectory(bridge/tests)
  else()
    message(STATUS "FFmpeg backend not built; its integration tests are skipped")
  endif()
endif()
