# VcpkgAutoDetect.cmake
# Automatically detect vcpkg installation and set CMAKE_TOOLCHAIN_FILE
#
# This module searches for vcpkg in common locations and sets:
#   VCPKG_ROOT - Path to vcpkg installation
#   CMAKE_TOOLCHAIN_FILE - Path to vcpkg.cmake toolchain file
#
# Search priority:
#   1. VCPKG_ROOT environment variable
#   2. Common user directories
#   3. System-wide installations
#   4. Project-relative locations

if(NOT DEFINED CACHE{VCPKG_ROOT})
    # Try environment variable first
    if(DEFINED ENV{VCPKG_ROOT})
        set(VCPKG_ROOT "$ENV{VCPKG_ROOT}" CACHE PATH "Path to vcpkg installation")
        message(STATUS "VCPKG_ROOT from environment: ${VCPKG_ROOT}")
    else()
        # Try common locations (ordered by likelihood)
        # Paths are platform-aware: Unix paths for Linux/macOS, Windows paths for Windows
        set(_VCPKG_SEARCH_PATHS
            # User development directories (Unix-style)
            "$ENV{HOME}/git/vcpkg"
            "$ENV{HOME}/vcpkg"
            "$ENV{HOME}/src/vcpkg"
            "$ENV{HOME}/workspace/vcpkg"
            "$ENV{HOME}/dev/vcpkg"
            "$ENV{HOME}/.vcpkg"
            # User-specific paths (in case HOME not set)
            "/home/$ENV{USER}/git/vcpkg"
            "/home/$ENV{USER}/vcpkg"
            # System-wide installations (Unix)
            "/opt/vcpkg"
            "/usr/local/vcpkg"
            "/usr/local/share/vcpkg"
            # Windows paths accessible via WSL
            "/mnt/c/vcpkg"
            "/mnt/c/dev/vcpkg"
            "/mnt/c/src/vcpkg"
            "/mnt/c/Users/$ENV{USER}/vcpkg"
            "/mnt/c/Users/$ENV{USER}/git/vcpkg"
            # Windows-native paths (when running CMake on Windows directly)
            "C:/vcpkg"
            "C:/dev/vcpkg"
            "C:/src/vcpkg"
            "$ENV{USERPROFILE}/vcpkg"
            "$ENV{USERPROFILE}/git/vcpkg"
            "$ENV{USERPROFILE}/source/vcpkg"
            "$ENV{LOCALAPPDATA}/vcpkg"
            "$ENV{PROGRAMFILES}/vcpkg"
            # Project-relative (if vcpkg is vendored) - works on all platforms
            "${CMAKE_SOURCE_DIR}/vcpkg"
            "${CMAKE_SOURCE_DIR}/external/vcpkg"
            "${CMAKE_SOURCE_DIR}/third_party/vcpkg"
            "${CMAKE_SOURCE_DIR}/../vcpkg"
        )

        foreach(_PATH ${_VCPKG_SEARCH_PATHS})
            if(EXISTS "${_PATH}/scripts/buildsystems/vcpkg.cmake")
                set(VCPKG_ROOT "${_PATH}" CACHE PATH "Path to vcpkg installation")
                message(STATUS "VCPKG_ROOT auto-detected: ${VCPKG_ROOT}")
                break()
            endif()
        endforeach()

        if(NOT DEFINED CACHE{VCPKG_ROOT})
            message(WARNING "VCPKG_ROOT not found. Searched: ${_VCPKG_SEARCH_PATHS}")
        endif()
    endif()
endif()

# Set toolchain file if using vcpkg and not already set
if(DEFINED CACHE{VCPKG_ROOT} AND NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "CMake toolchain file")
    message(STATUS "CMAKE_TOOLCHAIN_FILE set to: ${CMAKE_TOOLCHAIN_FILE}")
endif()
