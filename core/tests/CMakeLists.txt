# Core unit tests (doctest). Header-only framework fetched at configure time
# into the build directory; no vendored source in the repository.
set(DOCTEST_VERSION 2.4.12)
set(DOCTEST_URL
  "https://github.com/doctest/doctest/archive/refs/tags/v${DOCTEST_VERSION}.tar.gz")
set(DOCTEST_DIR "${CMAKE_BINARY_DIR}/third_party/doctest-${DOCTEST_VERSION}")

if(NOT EXISTS "${DOCTEST_DIR}/doctest/doctest.h")
  file(DOWNLOAD ${DOCTEST_URL} "${CMAKE_BINARY_DIR}/third_party/doctest.tar.gz"
       SHOW_PROGRESS)
  file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/third_party/doctest.tar.gz"
       DESTINATION "${CMAKE_BINARY_DIR}/third_party")
endif()

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${DOCTEST_DIR}")

function(velmux_core_test name)
  add_executable(${name} ${name}.cpp doctest_main.cpp)
  target_link_libraries(${name} PRIVATE velmux::core velmux::internal doctest)
  target_compile_definitions(${name} PRIVATE
    VELMUX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../testdata")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

velmux_core_test(feature_registry_tests)
velmux_core_test(capability_registry_tests)
velmux_core_test(compression_tests)
velmux_core_test(planner_tests)
velmux_core_test(scheduler_tests)
velmux_core_test(job_store_tests)
velmux_core_test(recovery_tests)
velmux_core_test(engine_tests)
velmux_core_test(session_tests)
