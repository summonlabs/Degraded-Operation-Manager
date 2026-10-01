cmake_minimum_required(VERSION 3.20)

project(DegradedOperationManager
  VERSION 1.0.0
  DESCRIPTION "Degraded Operation Manager - facility-wide degraded operating mode authority"
  HOMEPAGE_URL "https://github.com/summonlabs/Degraded-Operation-Manager"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(DOM_BUILD_RUNTIME "Build the durable state store and coordinator runtime" ON)
option(DOM_BUILD_TESTS "Build the Degraded Operation Manager test suites" ON)
option(DOM_BUILD_TOOLS "Build the Degraded Operation Manager command line tools" ON)
option(DOM_BUILD_EXAMPLES "Build the Degraded Operation Manager examples" ON)
option(DOM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(DOM_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(DOM_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(DOM_DISCOVER_SANITIZER_SUPPORT "Probe the toolchain for sanitizer support" ON)
option(DOM_ENABLE_ANALYZE "Run MSVC static analysis (/analyze) over first-party sources" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(DomWarnings)
include(DomSanitizers)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(DOM_CORE_SOURCES
  src/core/status.cpp
  src/core/digest.cpp
  src/core/reason.cpp
  src/domain/scalar.cpp
  src/domain/evidence.cpp
  src/domain/policy.cpp
  src/domain/decision.cpp
  src/domain/authority.cpp
  src/codec/binary_writer.cpp
  src/codec/binary_reader.cpp
  src/codec/codec_binary.cpp
  src/codec/codec_text.cpp
  src/engine/engine.cpp
)

add_library(dom_core STATIC ${DOM_CORE_SOURCES})
add_library(DegradedOperationManager::dom_core ALIAS dom_core)
target_include_directories(dom_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Private headers under src/ are implementation detail and are never installed.
target_include_directories(dom_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(dom_core PUBLIC DOM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR})
set_target_properties(dom_core PROPERTIES OUTPUT_NAME dom_core)
dom_apply_warnings(dom_core)
dom_apply_sanitizers(dom_core)

if(DOM_BUILD_RUNTIME)

set(DOM_RUNTIME_SOURCES
  src/store/file_io.cpp
  src/store/lock.cpp
  src/store/snapshot.cpp
  src/store/journal.cpp
  src/store/state_store.cpp
  src/runtime/coordinator.cpp
)

add_library(dom_runtime STATIC ${DOM_RUNTIME_SOURCES})
add_library(DegradedOperationManager::dom_runtime ALIAS dom_runtime)
target_include_directories(dom_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Private headers under src/ are implementation detail and are never installed.
target_include_directories(dom_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dom_runtime PUBLIC dom_core Threads::Threads)
set_target_properties(dom_runtime PROPERTIES OUTPUT_NAME dom_runtime)
dom_apply_warnings(dom_runtime)
dom_apply_sanitizers(dom_runtime)

add_library(DegradedOperationManager::dom ALIAS dom_runtime)

endif()

if(DOM_BUILD_RUNTIME AND DOM_BUILD_TOOLS)
  set(DOM_TOOLS dom_cli dom_authority)
  foreach(tool IN LISTS DOM_TOOLS)
    add_executable(${tool} tools/${tool}_main.cpp)
    target_link_libraries(${tool} PRIVATE dom_runtime)
    dom_apply_warnings(${tool})
    dom_apply_sanitizers(${tool})
  endforeach()
  install(TARGETS ${DOM_TOOLS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(DOM_BUILD_RUNTIME AND DOM_BUILD_EXAMPLES)
  add_executable(dom_example_walkthrough examples/walkthrough.cpp)
  target_link_libraries(dom_example_walkthrough PRIVATE dom_runtime)
  dom_apply_warnings(dom_example_walkthrough)
  dom_apply_sanitizers(dom_example_walkthrough)
endif()

if(DOM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

set(DOM_INSTALL_TARGETS dom_core)
if(DOM_BUILD_RUNTIME)
  list(APPEND DOM_INSTALL_TARGETS dom_runtime)
endif()

install(TARGETS ${DOM_INSTALL_TARGETS}
  EXPORT DegradedOperationManagerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT DegradedOperationManagerTargets
  FILE DegradedOperationManagerTargets.cmake
  NAMESPACE DegradedOperationManager::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DegradedOperationManager)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DegradedOperationManagerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedOperationManagerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DegradedOperationManager)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedOperationManagerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedOperationManagerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/DegradedOperationManagerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DegradedOperationManager)
