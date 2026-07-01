# Chainloaded toolchain for ccache integration
# This file is loaded by vcpkg triplets. ccache is used as the compiler launcher
# only when it is available on PATH; otherwise this is a no-op so the build works
# on machines without ccache installed.

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
