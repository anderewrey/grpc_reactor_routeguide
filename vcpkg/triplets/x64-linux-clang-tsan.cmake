set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
# Static: every dependency is linked into the test binaries, so no uninstrumented .so can be
# picked up at runtime instead.
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_BUILD_TYPE release)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../toolchains/ccache-toolchain.cmake")

# TSan only tracks synchronization it can see, so every dependency must be instrumented: an
# uninstrumented gRPC/Abseil makes its internal locking invisible and produces false races.
# No -march=native: these binaries are cached and restored on other runners.
set(VCPKG_C_FLAGS "-fsanitize=thread -fno-omit-frame-pointer -g")
set(VCPKG_CXX_FLAGS "-fsanitize=thread -fno-omit-frame-pointer -g")
set(VCPKG_LINKER_FLAGS "-fsanitize=thread")

# -O1 without -DNDEBUG, as in x64-linux-clang-asan.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_C_COMPILER=clang"
    "-DCMAKE_CXX_COMPILER=clang++"
    "-DCMAKE_C_FLAGS_RELEASE=-O1"
    "-DCMAKE_CXX_FLAGS_RELEASE=-O1")
