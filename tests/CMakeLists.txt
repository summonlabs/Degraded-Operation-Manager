# Degraded Operation Manager - test suites.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# No test declares a timeout: a hanging case is a defect to diagnose, never
# something to hide behind a watchdog.

add_library(dom_testkit STATIC
  testkit/testkit.cpp
  testkit/process.cpp
  testkit/fixtures.cpp)
target_include_directories(dom_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dom_testkit PUBLIC dom_runtime)
dom_apply_warnings(dom_testkit)
dom_apply_sanitizers(dom_testkit)

set(DOM_TEST_SUITES
  dom_test_unit
  dom_test_codec
  dom_test_engine
  dom_test_property
  dom_test_persistence
  dom_test_concurrency
  dom_test_multiprocess
  dom_test_integration
  dom_test_scale)

foreach(suite IN LISTS DOM_TEST_SUITES)
  add_executable(${suite} ${suite}.cpp)
  target_link_libraries(${suite} PRIVATE dom_testkit)
  dom_apply_warnings(${suite})
  dom_apply_sanitizers(${suite})
  add_test(NAME ${suite} COMMAND ${suite})
endforeach()

# Helper process used by the multiprocess and crash suites. It is not a test:
# it is the independent process that is locked out, killed at publication
# boundaries, and reopened from.
add_executable(dom_test_child dom_test_child.cpp)
target_link_libraries(dom_test_child PRIVATE dom_testkit)
dom_apply_warnings(dom_test_child)
dom_apply_sanitizers(dom_test_child)
