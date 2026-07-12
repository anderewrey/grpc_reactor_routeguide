# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 anderewrey

# CompilerLauncher.cmake
# Configures and reports the compiler launcher for this project's own targets.
#

if(CMAKE_CXX_COMPILER_LAUNCHER)
    message(STATUS "Compiler launcher: ${CMAKE_CXX_COMPILER_LAUNCHER} (from preset or environment)")
else()
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "Compiler launcher: ${CCACHE_PROGRAM} (ccache auto-detected)")
    else()
        message(STATUS "Compiler launcher: none (ccache not found)")
    endif()
endif()
