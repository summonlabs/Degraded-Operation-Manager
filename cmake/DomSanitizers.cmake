# Degraded Operation Manager - sanitizer capability probing and application.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0
#
# The probe is a one-shot compile/link/run of a tiny translation unit. When the
# toolchain cannot provide a sanitizer, configuration records the exact missing
# component and marks that coverage UNSUPPORTED on this host instead of silently
# pretending the build is instrumented.

set(DOM_ASAN_STATUS "NOT_REQUESTED" CACHE INTERNAL "AddressSanitizer capability on this host")
set(DOM_UBSAN_STATUS "NOT_REQUESTED" CACHE INTERNAL "UndefinedBehaviorSanitizer capability on this host")

if(MSVC)
  set(DOM_ASAN_FLAG "/fsanitize=address" CACHE INTERNAL "AddressSanitizer compiler flag")
  set(DOM_UBSAN_FLAG "/fsanitize=undefined" CACHE INTERNAL "UndefinedBehaviorSanitizer compiler flag")
else()
  set(DOM_ASAN_FLAG "-fsanitize=address" CACHE INTERNAL "AddressSanitizer compiler flag")
  set(DOM_UBSAN_FLAG "-fsanitize=undefined" CACHE INTERNAL "UndefinedBehaviorSanitizer compiler flag")
endif()

function(dom_probe_sanitizer flag out_var)
  if(NOT DOM_DISCOVER_SANITIZER_SUPPORT)
    set(${out_var} "UNPROBED" PARENT_SCOPE)
    return()
  endif()
  include(CheckCXXSourceRuns)
  set(CMAKE_REQUIRED_FLAGS "${flag}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${flag}")
  unset(SANITIZER_PROBE CACHE)
  check_cxx_source_runs("int main() { return 0; }" SANITIZER_PROBE)
  set(probe_ok ${SANITIZER_PROBE})
  unset(SANITIZER_PROBE CACHE)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(probe_ok)
    set(${out_var} "SUPPORTED" PARENT_SCOPE)
  else()
    set(${out_var} "UNSUPPORTED" PARENT_SCOPE)
  endif()
endfunction()

if(DOM_ENABLE_ASAN)
  dom_probe_sanitizer("${DOM_ASAN_FLAG}" dom_asan_probe)
  set(DOM_ASAN_STATUS "${dom_asan_probe}" CACHE INTERNAL "AddressSanitizer capability on this host")
  if(NOT dom_asan_probe STREQUAL "SUPPORTED")
    message(FATAL_ERROR
      "DOM_ENABLE_ASAN was requested but ${DOM_ASAN_FLAG} is not usable with this toolchain. "
      "AddressSanitizer coverage is UNSUPPORTED on this host.")
  endif()
endif()

if(DOM_ENABLE_UBSAN)
  dom_probe_sanitizer("${DOM_UBSAN_FLAG}" dom_ubsan_probe)
  set(DOM_UBSAN_STATUS "${dom_ubsan_probe}" CACHE INTERNAL "UndefinedBehaviorSanitizer capability on this host")
  if(NOT dom_ubsan_probe STREQUAL "SUPPORTED")
    message(FATAL_ERROR
      "DOM_ENABLE_UBSAN was requested but ${DOM_UBSAN_FLAG} is not usable with this toolchain. "
      "UndefinedBehaviorSanitizer coverage is UNSUPPORTED on this host.")
  endif()
endif()

function(dom_apply_sanitizers target)
  if(DOM_ENABLE_ASAN)
    # AddressSanitizer reports accurate stacks only with debug information, and
    # MSVC warns (C5072) when it is missing. A sanitizer build is therefore
    # always a debug-information build, whatever optimisation level is chosen.
    if(MSVC)
      target_compile_options(${target} PRIVATE /Zi)
    else()
      target_compile_options(${target} PRIVATE -g)
    endif()
    target_compile_options(${target} PRIVATE ${DOM_ASAN_FLAG})
    target_link_options(${target} PRIVATE ${DOM_ASAN_FLAG})
  endif()
  if(DOM_ENABLE_UBSAN)
    target_compile_options(${target} PRIVATE ${DOM_UBSAN_FLAG})
    target_link_options(${target} PRIVATE ${DOM_UBSAN_FLAG})
  endif()
endfunction()

message(STATUS "DOM sanitizer probe: ASan=${DOM_ASAN_STATUS} UBSan=${DOM_UBSAN_STATUS}")
