set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
# Static: every dependency is linked into the test binaries, so no uninstrumented .so can be
# picked up at runtime instead.
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_BUILD_TYPE release)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../toolchains/ccache-toolchain.cmake")

# The whole gRPC bundle (gRPC, Protobuf, Abseil, c-ares, RE2), OpenSSL and zlib get the same ASan
# instrumentation as the project, so headers and prebuilt libraries agree on every sanitizer-
# dependent layout. No -march=native: these binaries are cached and restored on other runners.
set(VCPKG_C_FLAGS "-fsanitize=address -fno-omit-frame-pointer -g")
set(VCPKG_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer -g")
set(VCPKG_LINKER_FLAGS "-fsanitize=address")

# -O1 without -DNDEBUG: keeps the debug-only assertions of gRPC, Abseil and Protobuf active
# (GPR_DEBUG_ASSERT, ABSL_DCHECK), and matches the project's own Debug build, which has no NDEBUG.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_C_COMPILER=clang"
    "-DCMAKE_CXX_COMPILER=clang++"
    "-DCMAKE_C_FLAGS_RELEASE=-O1"
    "-DCMAKE_CXX_FLAGS_RELEASE=-O1")
