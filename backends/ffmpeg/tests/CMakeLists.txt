# FFmpeg backend integration tests. These run REAL conversions through the
# libav* libraries and validate outputs with an INDEPENDENT ffprobe
# cross-check (spec §50: never validate only with the code that produced
# the output). Requires the Velmux-built host FFmpeg prefix (which ships
# ffprobe as a dev tool).
set(FFPROBE "${VELMUX_FFMPEG_PREFIX}/bin/ffprobe")
if(NOT EXISTS "${FFPROBE}")
  find_program(FFPROBE_BIN ffprobe)
  if(FFPROBE_BIN)
    set(FFPROBE "${FFPROBE_BIN}")
  endif()
endif()

function(velmux_ffmpeg_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE velmux::core velmux::backend-ffmpeg
                                        doctest)
  target_compile_definitions(${name} PRIVATE
    VELMUX_TEST_FFPROBE="${FFPROBE}"
    VELMUX_TEST_FFMPEG_PREFIX="${VELMUX_FFMPEG_PREFIX}")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endfunction()

message(STATUS "FFmpeg integration tests will cross-validate with: ${FFPROBE}")

velmux_ffmpeg_test(ffmpeg_backend_tests)
