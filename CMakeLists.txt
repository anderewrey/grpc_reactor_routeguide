cmake_minimum_required(VERSION 3.25)
project(grpc-reactor-routeguide LANGUAGES CXX)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 20)

message(STATUS "Using system-installed libraries")

# CRITICAL: When using system libraries, verify that gRPC/Protobuf/Abseil were built
# with the same compiler to avoid template instantiation incompatibilities.

if(NOT SKIP_COMPILER_CHECK)
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
else()
    message(WARNING "Compiler check bypassed via SKIP_COMPILER_CHECK=ON. Ensure system libraries match your compiler!")
    # gRPC/Protobuf/Abseil libraries are installed in /usr/local
    set(GRPC_PREFIX "/usr/local" CACHE PATH "gRPC installation prefix")
endif()

message(STATUS "Using compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Help find_package locate libraries
list(PREPEND CMAKE_PREFIX_PATH "${GRPC_PREFIX}")

# Configure RPATH for runtime shared library loading
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
# For system libraries, set explicit RPATH
set(CMAKE_BUILD_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")
set(CMAKE_INSTALL_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")
# Silence warnings about library conflicts between /usr/local and system directories
list(APPEND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${GRPC_PREFIX}/lib" "${GRPC_PREFIX}/lib64")

if(POLICY CMP0042)
    cmake_policy(SET CMP0042 NEW)  # Enable RPATH by default on macOS
endif()

# Binary Size Optimizations for Release/MinSizeRel builds
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    # Enable function/data sections for dead code elimination
    add_compile_options(-ffunction-sections -fdata-sections)

    # Enable linker garbage collection and strip symbols
    add_link_options(-Wl,--gc-sections -s)

    message(STATUS "Binary size optimizations enabled: function sections, gc-sections, symbol stripping")
endif()

add_subdirectory(proto)
add_subdirectory(common)
add_subdirectory(client)
add_subdirectory(server)
