# CompilerCompatibilityCheck.cmake
# Verifies compiler compatibility between system-installed libraries and current compiler
#
# This module checks that gRPC/Protobuf/Abseil system libraries were built with the same
# compiler type (GCC vs Clang) to avoid template instantiation incompatibilities.
#
# Input variables:
#   USING_VCPKG - Set to TRUE if using vcpkg (skips check)
#   SKIP_COMPILER_CHECK - Set to TRUE to bypass check (not recommended)
#
# Output variables:
#   GRPC_PREFIX - Path to gRPC installation (set to /usr/local)

# Detect if using vcpkg (vcpkg sets VCPKG_TOOLCHAIN or CMAKE_TOOLCHAIN_FILE contains "vcpkg")
if(DEFINED VCPKG_TOOLCHAIN OR (DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg"))
    message(STATUS "Using vcpkg-provided libraries (skipping system library checks)")
    set(USING_VCPKG TRUE)
else()
    message(STATUS "Using system-installed libraries")
    set(USING_VCPKG FALSE)
endif()

# CRITICAL: When using system libraries, verify that gRPC/Protobuf/Abseil were built
# with the same compiler to avoid template instantiation incompatibilities.

if(NOT USING_VCPKG AND NOT SKIP_COMPILER_CHECK)
    # gRPC/Protobuf/Abseil libraries are installed in /usr/local
    set(GRPC_PREFIX "/usr/local" CACHE PATH "gRPC installation prefix")

    # Find the actual libraries to check
    set(LIBS_TO_CHECK
        "${GRPC_PREFIX}/lib/libgrpc++.so"
        "${GRPC_PREFIX}/lib64/libprotobuf.so"
        "${GRPC_PREFIX}/lib64/libabsl_base.so"
    )

    set(COMPILER_MISMATCH FALSE)
    set(MISMATCH_DETAILS "")

    foreach(LIB_PATH ${LIBS_TO_CHECK})
        if(EXISTS "${LIB_PATH}")
            # Extract compiler info from the library's .comment section
            execute_process(
                COMMAND readelf -p .comment "${LIB_PATH}"
                OUTPUT_VARIABLE LIB_COMMENT
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            # Detect library compiler type and version
            set(LIB_COMPILER_TYPE "unknown")
            set(LIB_COMPILER_VERSION "unknown")

            if(LIB_COMMENT MATCHES "GCC: \\(GNU\\) ([0-9]+)\\.([0-9]+)")
                set(LIB_COMPILER_TYPE "GNU")
                set(LIB_COMPILER_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
            elseif(LIB_COMMENT MATCHES "clang version ([0-9]+)\\.([0-9]+)")
                set(LIB_COMPILER_TYPE "Clang")
                set(LIB_COMPILER_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
            endif()

            # Get current compiler info
            if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
                set(CURRENT_COMPILER_TYPE "GNU")
                string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" CURRENT_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
            elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                set(CURRENT_COMPILER_TYPE "Clang")
                string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" CURRENT_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
            else()
                set(CURRENT_COMPILER_TYPE "${CMAKE_CXX_COMPILER_ID}")
                set(CURRENT_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
            endif()

            # Compare compilers
            if(NOT "${LIB_COMPILER_TYPE}" STREQUAL "${CURRENT_COMPILER_TYPE}")
                set(COMPILER_MISMATCH TRUE)
                string(APPEND MISMATCH_DETAILS
                    "  ${LIB_PATH}:\n"
                    "    Built with: ${LIB_COMPILER_TYPE} ${LIB_COMPILER_VERSION}\n"
                    "    Your compiler: ${CURRENT_COMPILER_TYPE} ${CURRENT_COMPILER_VERSION}\n"
                )
            elseif(NOT "${LIB_COMPILER_VERSION}" STREQUAL "${CURRENT_COMPILER_VERSION}")
                # Major.minor version mismatch is a warning, not fatal
                message(WARNING
                    "Compiler version mismatch for ${LIB_PATH}:\n"
                    "  Library built with: ${LIB_COMPILER_TYPE} ${LIB_COMPILER_VERSION}\n"
                    "  Your compiler: ${CURRENT_COMPILER_TYPE} ${CURRENT_COMPILER_VERSION}\n"
                    "  This may cause issues with template-heavy code."
                )
            endif()

            message(STATUS "Checked ${LIB_PATH}: ${LIB_COMPILER_TYPE} ${LIB_COMPILER_VERSION}")
        endif()
    endforeach()

    # Fatal error if compiler type mismatch (GCC vs Clang)
    if(COMPILER_MISMATCH)
        message(FATAL_ERROR
            "================================================================================\n"
            "ERROR: Compiler mismatch detected!\n"
            "Your system libraries (gRPC/Protobuf/Abseil) were built with a different\n"
            "compiler than the one you're using. This WILL cause undefined reference\n"
            "errors due to template instantiation incompatibilities.\n"
            "Mismatches found:\n"
            "${MISMATCH_DETAILS}"
            "SOLUTIONS:\n"
            "  1. Switch to the compiler used to build system libraries:\n"
            "     cmake -B build -DCMAKE_CXX_COMPILER=<matching-compiler>\n"
            "  2. Rebuild system libraries with your current compiler\n"
            "     (See GRPC_BUILD_GUIDE.md)\n"
            "  3. Bypass this check (NOT RECOMMENDED, will likely fail at link time):\n"
            "     cmake -B build -DSKIP_COMPILER_CHECK=ON\n"
            "See GRPC_BUILD_GUIDE.md for detailed explanation.\n"
            "================================================================================")
    endif()

    message(STATUS "Compiler compatibility check passed: ${CURRENT_COMPILER_TYPE} ${CURRENT_COMPILER_VERSION}")
elseif(NOT USING_VCPKG)
    message(WARNING "Compiler check bypassed via SKIP_COMPILER_CHECK=ON. Ensure system libraries match your compiler!")
    # gRPC/Protobuf/Abseil libraries are installed in /usr/local
    set(GRPC_PREFIX "/usr/local" CACHE PATH "gRPC installation prefix")
endif()

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
