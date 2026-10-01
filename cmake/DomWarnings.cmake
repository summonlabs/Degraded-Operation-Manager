# Degraded Operation Manager - strict compiler warning configuration.
#
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

function(dom_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /Zc:preprocessor
      /EHsc
      /MP
    )
    if(DOM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(DOM_ENABLE_ANALYZE)
      # Static analysis findings are reviewed by hand; analysis warnings are not
      # promoted to errors because several C6xxx findings are stylistic.
      target_compile_options(${target} PRIVATE /analyze)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wold-style-cast
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
    )
    if(DOM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
