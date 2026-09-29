# Chainloaded toolchain for ccache integration
# This file is loaded by vcpkg triplets. ccache is used as the compiler launcher
# only when it is available on PATH; otherwise this is a no-op so the build works
# on machines without ccache installed.

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# A chainloaded toolchain replaces vcpkg's own scripts/toolchains/linux.cmake, which is what applies
# a triplet's VCPKG_C_FLAGS/VCPKG_CXX_FLAGS/VCPKG_LINKER_FLAGS. Without these lines, the sanitizer
# triplets' -fsanitize flags never reach any CMake-built dependency. The triplets that do not set
# them are unaffected: the variables are empty.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VCPKG_C_FLAGS VCPKG_CXX_FLAGS VCPKG_LINKER_FLAGS)
string(APPEND CMAKE_C_FLAGS_INIT " ${VCPKG_C_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${VCPKG_CXX_FLAGS}")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS}")
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS}")
string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS}")
