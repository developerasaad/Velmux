# C ABI integration tests: exercise the versioned C facade exactly the way
# the Kotlin engine clients will (JSON in, JSON out) against the REAL engine
# + FFmpeg processing module. Outputs are cross-validated with ffprobe.
set(FFPROBE "${VELMUX_FFMPEG_PREFIX}/bin/ffprobe")
if(NOT EXISTS "${FFPROBE}")
  find_program(FFPROBE_BIN ffprobe)
  if(FFPROBE_BIN)
    set(FFPROBE "${FFPROBE_BIN}")
  endif()
endif()

add_executable(abi_tests bridge_tests.cpp
               ${CMAKE_SOURCE_DIR}/core/tests/doctest_main.cpp)
target_link_libraries(abi_tests PRIVATE velmux::abi doctest)
target_compile_definitions(abi_tests PRIVATE
  VELMUX_TEST_FFPROBE="${FFPROBE}"
  VELMUX_TEST_FFMPEG_PREFIX="${VELMUX_FFMPEG_PREFIX}")
add_test(NAME abi_tests COMMAND abi_tests)
set_tests_properties(abi_tests PROPERTIES TIMEOUT 600)

message(STATUS "ABI tests will cross-validate with: ${FFPROBE}")
