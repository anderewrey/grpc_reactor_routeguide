set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Build only Release configuration
set(VCPKG_BUILD_TYPE release)

# Chainload toolchain with ccache
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../toolchains/ccache-toolchain.cmake")

# GCC 13 specific settings with optimizations
# Uses GCC 13 from Red Hat toolset at /opt/rh/gcc-toolset-13/root/usr/bin
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_C_COMPILER=/opt/rh/gcc-toolset-13/root/usr/bin/gcc"
    "-DCMAKE_CXX_COMPILER=/opt/rh/gcc-toolset-13/root/usr/bin/g++"
    "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -march=native -mtune=native -g0"
    "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -march=native -mtune=native -g0"
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON")
