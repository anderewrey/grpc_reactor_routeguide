# gRPC build and optimization guide

## Overview

This project builds its gRPC/Protobuf dependency stack through vcpkg (see [vcpkg-usage.md](/docs/vcpkg-usage.md)).
This guide documents the underlying manual build approach instead: how to build gRPC, Protobuf, and Abseil directly
from source with CMake, independent of any package manager. gRPC's own documentation covers this poorly, so this
guide exists for anyone building the stack manually, on this project or elsewhere.

**Last verified with**: gRPC v1.73.1, Protobuf v31.0, Abseil 20250127

### The gRPC bundle

**Key principle**: gRPC, Protobuf, Abseil, c-ares, and RE2 should be treated as a **single bundle**:

- All five libraries are tightly coupled and must match versions.
- All must be built from gRPC's git repository (using submodules), not installed separately.
- All must be built with the same compiler and optimization flags.

### Two ways to get gRPC

1. **System package manager** (`apt`/`dnf`): easiest, but compiler and optimization flags are out of your control,
   and versions may not match across the bundle.
2. **Manual build from source** (this guide): full control over compiler and optimization, at the cost of a
   15-30 minute build.

## Quick start

### System dependencies

Only two system packages are required (everything else builds from gRPC's submodules): OpenSSL and zlib
development headers, installed with your distribution's package manager.

| Package | dnf-based | apt-based |
| ------- | --------- | --------- |
| OpenSSL headers | `openssl-devel` | `libssl-dev` |
| zlib headers | `zlib-devel` | `zlib1g-dev` |

Do **not** install `c-ares-devel`, `re2-devel`, `abseil-cpp-devel`, or `protobuf-devel` from the system: gRPC needs
these built from its own submodules so that versions and compiler flags match across the bundle.

### Build command

```bash
cd ~/git
git clone --depth 1 --branch v1.73.1 https://github.com/grpc/grpc.git
cd grpc
git submodule update --init

cmake -B build-shared-speed \
  -GNinja \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=20 \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DgRPC_INSTALL=ON \
  -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
  -DgRPC_BUILD_CODEGEN=ON \
  -DgRPC_SSL_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package \
  -DgRPC_PROTOBUF_PROVIDER=module \
  -DgRPC_ABSL_PROVIDER=module \
  -DgRPC_CARES_PROVIDER=module \
  -DgRPC_RE2_PROVIDER=module \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_WITH_ZLIB=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local

ninja -C build-shared-speed -j$(nproc)
sudo ninja -C build-shared-speed install
sudo ldconfig
```

This builds C++-only, Release, with shared libraries and speed optimizations. See the options reference below for
what each flag controls, and the "Build variations" section for a debug build or a portable (non-`native`) build.

## CMake options reference

### Core build configuration

| Option | Default | Recommended | Purpose |
| ------ | ------- | ------------ | ------- |
| `CMAKE_BUILD_TYPE` | N/A | `Release` | Sets optimization level and defines |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | `/usr/local` | Where to install libraries/headers |
| `CMAKE_CXX_STANDARD` | `14` | `20` | C++ standard version (match your project) |
| `BUILD_SHARED_LIBS` | `OFF` | `ON` | Build .so instead of .a (smaller app binaries) |
| `CMAKE_C_COMPILER` | system default | `/usr/bin/gcc` | C compiler to use |
| `CMAKE_CXX_COMPILER` | system default | `/usr/bin/g++` | C++ compiler to use |

### Optimization flags

| Flag | Purpose | Recommended |
| ------ | --------- | ------------- |
| `-O3` | Maximum optimization | **YES** |
| `-march=native` | Use CPU-specific instructions | **YES**, unless the binary must run on other CPUs |
| `-mtune=native` | Tune for specific CPU | **YES**, unless the binary must run on other CPUs |
| `-DNDEBUG` | Remove assert() checks | **YES** for Release |
| `-g0` | No debug symbols | **YES** for Release |

### gRPC language plugin options

Only build what you need; each plugin adds build time.

| Option | Default | C++ Only | All Languages |
| -------- | --------- | ---------- | --------------- |
| `gRPC_BUILD_GRPC_CPP_PLUGIN` | `ON` | `ON` | `ON` |
| `gRPC_BUILD_GRPC_CSHARP_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_NODE_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_PHP_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_PYTHON_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_RUBY_PLUGIN` | `ON` | `OFF` | `ON` |

### gRPC core options

| Option | Default | Recommended | Purpose |
| -------- | --------- | ------------- | --------- |
| `gRPC_INSTALL` | `ON` | `ON` | Install to CMAKE_INSTALL_PREFIX |
| `gRPC_BUILD_TESTS` | `OFF` | `OFF` | Don't build gRPC's own tests |
| `gRPC_BUILD_CODEGEN` | `ON` | `ON` | Build protoc and grpc_cpp_plugin |
| `gRPC_BUILD_GRPCPP_OTEL_PLUGIN` | `OFF` | `OFF` | OpenTelemetry plugin (rarely needed) |
| `gRPC_DOWNLOAD_ARCHIVES` | `ON` | `ON` | Auto-download dependencies |

### Dependency provider options

`module` builds from gRPC's own submodules (self-contained, version-matched); `package` uses system-installed
packages.

| Dependency | Recommended | Reason |
| ------------ | ------------- | -------- |
| `gRPC_SSL_PROVIDER` | `package` | Use system OpenSSL (gets security updates) |
| `gRPC_ZLIB_PROVIDER` | `package` | System zlib is already optimized |
| `gRPC_CARES_PROVIDER` | `module` | Must be version-matched to the bundle |
| `gRPC_RE2_PROVIDER` | `module` | Must be version-matched to the bundle |
| `gRPC_ABSL_PROVIDER` | `module` | Must match Protobuf's Abseil version |
| `gRPC_PROTOBUF_PROVIDER` | `module` | Must be version-matched to the bundle |

Only OpenSSL and zlib use `package`: they are universal dependencies with stable ABIs. Everything else in the bundle
must come from gRPC's submodules together, or template instantiation mismatches will cause linker errors (see
"Compiler compatibility" below).

### Protobuf-specific options

| Option | Recommended | Purpose |
| -------- | ------------- | --------- |
| `protobuf_BUILD_TESTS` | `OFF` | Skip protobuf's own tests |
| `protobuf_BUILD_EXAMPLES` | `OFF` | Skip examples |
| `protobuf_BUILD_CONFORMANCE` | `OFF` | Skip conformance tests |
| `protobuf_BUILD_PROTOC_BINARIES` | `ON` | Build protoc compiler |
| `protobuf_BUILD_LIBPROTOC` | `ON` | Needed for codegen |
| `protobuf_WITH_ZLIB` | `ON` | Enable zlib compression |
| `protobuf_ALLOW_CCACHE` | `ON` if ccache installed | Speed up rebuilds |

### Advanced options

- `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` enables Link-Time Optimization (LTO): faster runtime and better dead code
  elimination, at the cost of a substantially longer build. Use it for production builds where the extra build time
  is acceptable.
- `CMAKE_C_COMPILER_LAUNCHER=ccache` / `CMAKE_CXX_COMPILER_LAUNCHER=ccache` route compilation through ccache for much
  faster rebuilds. Requires the `ccache` package installed; pair with `-Dprotobuf_ALLOW_CCACHE=ON`.

## Build variations

### Portable (different CPU architectures)

Drop the CPU-specific flags from the build command above:

```cmake
# Change from:
-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"
-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"

# To:
-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0"
-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g0"
```

This works on any x86-64 CPU instead of only the machine it was built on. Useful when distributing binaries.

### Debug build

```bash
cmake -B build-debug \
  -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_STANDARD=20 \
  -DBUILD_SHARED_LIBS=ON \
  -DgRPC_INSTALL=ON \
  -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_BUILD_CODEGEN=ON \
  -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
  -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
  -DgRPC_SSL_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package \
  -DgRPC_CARES_PROVIDER=module \
  -DgRPC_RE2_PROVIDER=module \
  -DgRPC_ABSL_PROVIDER=module \
  -DgRPC_PROTOBUF_PROVIDER=module

ninja -C build-debug -j$(nproc)
sudo ninja -C build-debug install
```

Produces a fully debuggable, unoptimized build: no `-O3`, no `-march=native`, full debug symbols.

## Rebuild your project

After gRPC/Protobuf are installed system-wide:

```bash
cd /path/to/your/project

cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_CXX_COMPILER=g++ \
      -GNinja

cmake --build build
```

Verify the binary links against the libraries you just built, not stale system ones:

```bash
ldd build/your_binary | grep -E "libprotobuf|libgrpc"
```

Should show paths under `/usr/local/lib` (or wherever `CMAKE_INSTALL_PREFIX` pointed).

## Reducing binary size

Two independent techniques combine to shrink application binaries:

**1. Compiler/linker flags** (applied to your application's own CMakeLists.txt):

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections -s)
endif()
```

`-ffunction-sections`/`-fdata-sections` put each function/variable in its own linker section; `--gc-sections` then
lets the linker discard unreferenced ones, and `-s` strips symbols from the final binary.

**2. Shared linking against gRPC/Protobuf** (`-DBUILD_SHARED_LIBS=ON` in the build command above): instead of
statically embedding gRPC/Protobuf/Abseil in every binary, link against the shared `.so` files installed by this
guide. This is the larger win of the two, and also means the libraries are shared in memory across processes and can
be updated without recompiling the application.

**Why not `-Os`**: this project prioritizes runtime speed over binary size, so it keeps `-O3` and relies on shared
linking to keep binaries small instead of trading away optimization.

## Library linking strategy

| Library Category | Type | Rationale |
| ------------------ | ------ | ----------- |
| gRPC + Protobuf + Abseil + c-ares + RE2 | Shared | Version-coupled; one copy in memory, updatable without rebuild |
| OpenSSL + zlib | System | Security updates, universal ABI |
| Light utilities (e.g. spdlog, gflags) | Static, optimized | Small, simple to deploy, no extra `.so` files |
| Your own project libraries | Static (OBJECT for shared code) | Avoids duplicating object code into multiple binaries |

For "light utilities", force optimization even in Debug builds of your own application, since it's third-party code
you're not debugging:

```cmake
target_compile_options(spdlog PRIVATE -O3 -DNDEBUG -march=native -mtune=native -g0)
```

## Compiler compatibility (critical!)

### The problem: template instantiation incompatibility

**Rule**: gRPC, Protobuf, Abseil, and your application **must all use the same compiler** (all GCC or all Clang).

**Why**: these libraries are extremely template-heavy. Different compilers generate different symbols for the same
template code, causing linker errors like:

```text
undefined reference to `absl::lts_20250127::log_internal::LogMessage::operator<<(unsigned long)'
```

### Manual verification

Check which compiler built your libraries and confirm they all agree:

```bash
readelf -p .comment /usr/local/lib/libgrpc++.so | grep -i "gcc\|clang"
readelf -p .comment /usr/local/lib64/libprotobuf.so | grep -i "gcc\|clang"
readelf -p .comment /usr/local/lib64/libabsl_base.so | grep -i "gcc\|clang"
```

All three should show the same compiler and (ideally) the same major version.

### If they mismatch

Either rebuild your application with the compiler the libraries were built with:

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++      # if libraries are GCC-built
cmake -B build -DCMAKE_CXX_COMPILER=clang++  # if libraries are Clang-built
```

or rebuild the whole gRPC stack with your preferred compiler (see "Quick start" above).

## Deployment considerations

### Shared libraries to deploy

From `/usr/local/lib`: `libgrpc++.so.1.73`, `libgrpc.so.48`, `libabsl_*.so`, `libaddress_sorting.so`, `libgpr.so`,
`libre2.so`, `libupb.so`.

From `/usr/local/lib64`: `libprotobuf.so.31`, `libc-ares.so`.

### Deployment methods

1. **System installation** (what this guide does): `sudo ninja install && sudo ldconfig`.
2. **`LD_LIBRARY_PATH`**: `export LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64:$LD_LIBRARY_PATH`.
3. **RPATH baked into the binary** at configure time:
   `set(CMAKE_INSTALL_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")`.

### Docker/container deployment

```dockerfile
FROM <base-image>  # your distribution's base image
RUN dnf install -y openssl-libs zlib  # or: apt install -y openssl libz1
COPY --from=builder /usr/local/lib/libgrpc*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libabsl*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib64/libprotobuf*.so* /usr/local/lib64/
COPY --from=builder /usr/local/lib64/libc-ares*.so* /usr/local/lib64/
RUN ldconfig
COPY --from=builder /app/build/applications/*/* /app/
CMD ["/app/route_guide_callback_server"]
```

## Alternative: system package manager

Using system packages loses the guarantees the bundle approach gives you: no version-matching across gRPC,
Protobuf, Abseil, c-ares, and RE2, and no control over which compiler or optimization flags built them. For
production or performance-critical applications, building from source as documented above is recommended instead.

| Component | dnf-based | apt-based |
| --------- | --------- | --------- |
| gRPC | `grpc-devel` | `libgrpc++-dev` |
| gRPC plugins | `grpc-plugins` | `protobuf-compiler-grpc` |
| Protobuf | `protobuf-devel` | `libprotobuf-dev` |
| Abseil | `abseil-cpp-devel` | `libabsl-dev` |
| c-ares | `c-ares-devel` | `libc-ares-dev` |
| RE2 | `re2-devel` | `libre2-dev` |

On dnf-based distributions, these packages typically live behind the EPEL and CRB/PowerTools repositories.

## Troubleshooting

**`Could not find package OpenSSL` / `Could not find package ZLIB`**: install the system dev packages from
"System Dependencies" above.

**Missing c-ares, RE2, Abseil, or Protobuf packages during configure**: ensure `gRPC_*_PROVIDER=module` for these
dependencies; they must not be resolved from system packages.

**`ninja: build stopped: subcommand failed`**: check the actual error above this message. Common causes are running
out of memory (reduce parallelism with `ninja -j4`) or a compiler too old to support C++20 concepts and ranges.

**LTO/IPO errors**: remove `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` and rebuild.

**`libprotobuf.so not found` at runtime, or `grpc_cpp_plugin: error while loading shared libraries`**:

```bash
echo -e "/usr/local/lib\n/usr/local/lib64" | sudo tee /etc/ld.so.conf.d/usr-local.conf
sudo ldconfig
ldd /usr/local/bin/grpc_cpp_plugin | grep "not found"  # should return nothing
```

**Version mismatch between gRPC and Protobuf**: rebuild both together from the same gRPC checkout (see "Quick start").

**Compiler mismatch**: see "Compiler compatibility" above.

## Multi-compiler setup

To keep multiple compiler-built copies side by side, install each to its own prefix:

```bash
# GCC 11
-DCMAKE_INSTALL_PREFIX=/usr/local/gcc11
-DCMAKE_C_COMPILER=/usr/bin/gcc
-DCMAKE_CXX_COMPILER=/usr/bin/g++

# GCC 13
-DCMAKE_INSTALL_PREFIX=/usr/local/gcc13
-DCMAKE_C_COMPILER=/opt/rh/gcc-toolset-13/root/usr/bin/gcc
-DCMAKE_CXX_COMPILER=/opt/rh/gcc-toolset-13/root/usr/bin/g++

# Clang
-DCMAKE_INSTALL_PREFIX=/usr/local/clang
-DCMAKE_C_COMPILER=/usr/bin/clang
-DCMAKE_CXX_COMPILER=/usr/bin/clang++
```

Then register each prefix with `ldconfig` so the right `.so` files can be found at runtime:

```bash
echo "/usr/local/gcc11/lib
/usr/local/gcc11/lib64" | sudo tee /etc/ld.so.conf.d/gcc11-local.conf
sudo ldconfig
```

Repeat for each prefix, then select which one to link against per-build via `CMAKE_CXX_COMPILER` and, if using
vcpkg, the matching triplet (see [vcpkg-usage.md](/docs/vcpkg-usage.md)).

## Verification

```bash
# Shared libraries are installed
ls -lh /usr/local/lib/libgrpc++.so*
ls -lh /usr/local/lib64/libprotobuf.so*

# Tool versions
/usr/local/bin/protoc --version
/usr/local/bin/grpc_cpp_plugin --version

# Library cache picked them up
ldconfig -p | grep -E "libgrpc|libprotobuf|libabsl"
```

Then build your project (see "Rebuild Your Project" above) and confirm with `ldd` that it links against these
libraries rather than any stale system copies.

## Summary

1. Treat gRPC, Protobuf, Abseil, c-ares, and RE2 as one version-matched bundle, built together from gRPC's
   submodules.
2. Build your application and the entire bundle with the same compiler.
3. Prefer shared libraries for the bundle (`-DBUILD_SHARED_LIBS=ON`) and static libraries for small utilities.
4. Use `-O3 -march=native -mtune=native` for speed; drop `-march=native -mtune=native` only if the binary needs to
   run on a different CPU.

See "Quick start" above for the full build command.

**Next Steps:**

- For reactor pattern documentation, see [reactor_client.md](/applications/reactor/reactor_client.md)
- For vcpkg's automated version of this same build, see [vcpkg-usage.md](/docs/vcpkg-usage.md)
