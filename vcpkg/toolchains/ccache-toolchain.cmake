# Chainloaded toolchain for ccache integration
# This file is loaded by vcpkg triplets to enable ccache for all builds

set(CMAKE_C_COMPILER_LAUNCHER ccache)
set(CMAKE_CXX_COMPILER_LAUNCHER ccache)
