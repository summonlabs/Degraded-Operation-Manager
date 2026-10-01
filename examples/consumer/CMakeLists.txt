cmake_minimum_required(VERSION 3.20)

project(DegradedOperationManagerConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent consumer of the installed Degraded Operation Manager package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The consumer only ever sees the installed package: config, version, targets
# and public headers.
find_package(DegradedOperationManager 1.0 CONFIG REQUIRED)

add_executable(dom_consumer main.cpp)
target_link_libraries(dom_consumer PRIVATE DegradedOperationManager::dom_runtime)
