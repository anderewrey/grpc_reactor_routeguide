# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 anderewrey

# Coverage.cmake
# Instruments the project for code coverage measurement.
#
# ENABLE_COVERAGE selects the instrumentation, not the report generator:
#   OFF  - no instrumentation (default)
#   gcov - GCC counters written to .gcno/.gcda, consumed by gcovr or by lcov/genhtml
#   llvm - Clang source-based coverage mapping, consumed by llvm-profdata/llvm-cov
#
# The two modes are compiler-bound, because each one's counter format is only readable by the
# matching toolchain: gcov requires GCC and llvm requires Clang. Clang can emit gcov-style counters
# too, but their format version tracks Clang's own release rather than GCC's, which makes them a
# frequent source of "unsupported .gcda version" failures in gcovr and lcov, so the mode rejects it.
#
# Report generation is deliberately not modelled here. Each tool needs a different post-processing
# pipeline over the same instrumented build, and .github/workflows/coverage.yml drives them.

set(ENABLE_COVERAGE "OFF" CACHE STRING "Code coverage instrumentation to build with: OFF, gcov, or llvm")
set_property(CACHE ENABLE_COVERAGE PROPERTY STRINGS OFF gcov llvm)

option(COVERAGE_MCDC "Add MC/DC instrumentation, ENABLE_COVERAGE=llvm only" ON)

if(ENABLE_COVERAGE STREQUAL "OFF")
    return()
endif()

# BuildOptimizations.cmake strips symbols (-s) and garbage-collects sections in these build types,
# which discards the counter sections and the debug info that both formats need to map counters
# back to source lines.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    message(FATAL_ERROR
            "ENABLE_COVERAGE=${ENABLE_COVERAGE} is incompatible with CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}. "
            "Use a Debug build for coverage.")
endif()

if(ENABLE_COVERAGE STREQUAL "gcov")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
                "ENABLE_COVERAGE=gcov requires GCC, but CMAKE_CXX_COMPILER_ID is "
                "${CMAKE_CXX_COMPILER_ID}. Use ENABLE_COVERAGE=llvm with Clang.")
    endif()

    # -fprofile-abs-path records the absolute source path in the .gcno, so gcov resolves sources no
    # matter which directory the report generator runs from. Without it, the reactor headers are
    # recorded relative to each test binary's object directory and get reported as unresolvable.
    #
    # -fprofile-update=atomic makes counter increments atomic. The reactor tests execute gRPC
    # callbacks on the gRPC thread pool while the test body runs on its own thread, so the same
    # counters in reactor_client.h are incremented concurrently, and the default non-atomic
    # increment silently loses counts.
    #
    # -O0 is explicit because CMAKE_CXX_FLAGS_DEBUG only carries -g: at any higher level, inlining
    # and basic-block reordering attribute counts to lines the code no longer executes in place.
    add_compile_options(--coverage -fprofile-abs-path -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
    message(STATUS "Coverage instrumentation enabled: gcov (GCC counters, .gcno/.gcda)")
elseif(ENABLE_COVERAGE STREQUAL "llvm")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
                "ENABLE_COVERAGE=llvm requires Clang, but CMAKE_CXX_COMPILER_ID is "
                "${CMAKE_CXX_COMPILER_ID}. Use ENABLE_COVERAGE=gcov with GCC.")
    endif()

    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -O0 -g)
    add_link_options(-fprofile-instr-generate)

    # MC/DC (modified condition/decision coverage) instruments each condition of a compound
    # decision independently, so it reports whether every condition was shown to independently
    # affect the outcome. Clang skips, with a warning, any decision holding more than six
    # conditions. It has no gcov equivalent, which is why it is a separate switch here.
    if(COVERAGE_MCDC)
        add_compile_options(-fcoverage-mcdc)
        message(STATUS "Coverage instrumentation enabled: llvm (source-based mapping, with MC/DC)")
    else()
        message(STATUS "Coverage instrumentation enabled: llvm (source-based mapping)")
    endif()
else()
    message(FATAL_ERROR "ENABLE_COVERAGE must be OFF, gcov, or llvm, but is '${ENABLE_COVERAGE}'.")
endif()
