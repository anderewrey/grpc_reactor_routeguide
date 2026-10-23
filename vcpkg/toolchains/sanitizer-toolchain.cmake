# Chainloaded toolchain for the sanitizer triplets (x64-linux-clang-asan, x64-linux-clang-tsan).
#
# A chainloaded toolchain replaces vcpkg's own scripts/toolchains/linux.cmake, which is what applies
# a triplet's VCPKG_C_FLAGS/VCPKG_CXX_FLAGS/VCPKG_LINKER_FLAGS (and -fPIC). Without it, the
# -fsanitize flags never reach any CMake-built dependency. Rather than re-implementing it, include
# vcpkg's own file, then add ccache like every other triplet. The vcpkg root comes from
# Z_VCPKG_ROOT_DIR, which vcpkg.cmake propagates into try_compile projects, or from _VCPKG_ROOT_DIR,
# which vcpkg passes to every port configure. Deriving it from CMAKE_TOOLCHAIN_FILE failed CMake's
# compiler checks (try_compile) in detect_compiler.
#
# The compiler is chosen by the triplets' VCPKG_CMAKE_CONFIGURE_OPTIONS, which vcpkg's
# detect_compiler also receives, so the compiler binary is part of every package's ABI hash.
#
# No include_guard: CMake reads a toolchain file several times per configure (CMakeSystem.cmake
# re-includes it), and each read must apply it again. linux.cmake guards itself the way vcpkg needs.

if(Z_VCPKG_ROOT_DIR)
    set(VCPKG_LINUX_TOOLCHAIN_FILE "${Z_VCPKG_ROOT_DIR}/scripts/toolchains/linux.cmake")
else()
    set(VCPKG_LINUX_TOOLCHAIN_FILE "${_VCPKG_ROOT_DIR}/scripts/toolchains/linux.cmake")
endif()
if(NOT EXISTS "${VCPKG_LINUX_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "vcpkg's linux.cmake not found: ${VCPKG_LINUX_TOOLCHAIN_FILE}")
endif()
include("${VCPKG_LINUX_TOOLCHAIN_FILE}")

include("${CMAKE_CURRENT_LIST_DIR}/ccache-toolchain.cmake")
