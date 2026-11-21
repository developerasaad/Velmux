# Bridge integration tests: exercise the stable C facade exactly the way
# Kotlin/Swift/JSI hosts will (JSON in, JSON out) against the REAL engine +
# FFmpeg backend. Outputs are independently cross-validated with ffprobe.
set(FFPROBE "${VELMUX_FFMPEG_PREFIX}/bin/ffprobe")
if(NOT EXISTS "${FFPROBE}")
  find_program(FFPROBE_BIN ffprobe)
  if(FFPROBE_BIN)
    set(FFPROBE "${FFPROBE_BIN}")
  endif()
endif()

add_executable(bridge_tests bridge_tests.cpp
               ${CMAKE_SOURCE_DIR}/core/tests/doctest_main.cpp)
target_link_libraries(bridge_tests PRIVATE velmux_bridge doctest)
target_include_directories(bridge_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/bridge/cpp)
target_compile_definitions(bridge_tests PRIVATE
  VELMUX_TEST_FFPROBE="${FFPROBE}"
  VELMUX_TEST_FFMPEG_PREFIX="${VELMUX_FFMPEG_PREFIX}")
add_test(NAME bridge_tests COMMAND bridge_tests)
set_tests_properties(bridge_tests PROPERTIES TIMEOUT 600)

message(STATUS "Bridge tests will cross-validate with: ${FFPROBE}")
