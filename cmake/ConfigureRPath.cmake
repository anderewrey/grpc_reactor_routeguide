# ConfigureRPath.cmake
# Configures library search paths and RPATH for runtime shared library loading
#
# This module sets up CMake prefix paths and RPATH configuration for finding
# and loading system-installed libraries at runtime.
#
# Input variables:
#   USING_VCPKG - Set to TRUE if using vcpkg (skips RPATH config)
#   GRPC_PREFIX - Path to gRPC installation (defaults to /usr/local)

# Configure library search paths (only for system-installed libraries, not vcpkg)
if(NOT USING_VCPKG)
    # Help find_package locate libraries
    list(PREPEND CMAKE_PREFIX_PATH "${GRPC_PREFIX}")

    # Configure RPATH for runtime shared library loading
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    # For system libraries, set explicit RPATH
    set(CMAKE_BUILD_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")
    set(CMAKE_INSTALL_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")
    # Silence warnings about library conflicts between /usr/local and system directories
    list(APPEND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${GRPC_PREFIX}/lib" "${GRPC_PREFIX}/lib64")
endif()

if(POLICY CMP0042)
    cmake_policy(SET CMP0042 NEW)  # Enable RPATH by default on macOS
endif()
