# gRPC Build and Optimization Guide

## Overview

This comprehensive guide covers building, optimizing, and deploying gRPC + Protobuf for this project. It includes:

- **Quick start** commands for common scenarios
- **Complete CMake options reference** for customization
- **Binary size optimization** strategies and results
- **Library linking** decisions and rationale
- **Compiler compatibility** requirements
- **Deployment** considerations

**Last Updated**: 2025-10-22 (gRPC v1.73.1, Protobuf v31.0, Abseil 20250127)

### The gRPC Bundle

**Key Principle**: gRPC, Protobuf, Abseil, c-ares, and RE2 should be treated as a **single bundle**:

- All five libraries are tightly coupled (must match versions)
- All built from gRPC's git repository (includes submodules)
- All built with same compiler and optimization flags
- All deployed as shared libraries for optimal performance

### Build Strategies

Two approaches to get gRPC:

1. **System Package Manager** (Ubuntu/AlmaLinux)
   - Easiest but least control
   - Compiler/optimization flags unknown
   - May have version mismatches

2. **Manual Build from Source** (Recommended)
   - Full control over compiler and optimization
   - Speed-optimized (`-O3 -march=native -mtune=native`)
   - Documented in this guide

## Quick Start

### System Dependencies (Required)

**Only two system packages required:**

```bash
# AlmaLinux 9 / RHEL 9
sudo dnf install -y openssl-devel zlib-devel

# Ubuntu/Debian
sudo apt install -y libssl-dev zlib1g-dev
```

**These are NOT needed** (built with gRPC from submodules):

- `c-ares-devel` / `libc-ares-dev`
- `re2-devel` / `libre2-dev`
- `abseil-cpp-devel` / `libabsl-dev`
- `protobuf-devel` / `libprotobuf-dev`

**Why this matters:**

- **Version compatibility**: gRPC bundles tested versions of all dependencies
- **Compiler consistency**: All built with same flags to avoid template instantiation issues
- **Self-contained**: No conflicts with system versions

### Recommended Build Command (C++ Only, Speed-Optimized)

```bash
cd ~/git
git clone --depth 1 --branch v1.73.1 https://github.com/grpc/grpc.git
cd grpc
git submodule update --init

cmake -B build-shared-speed \
  -GNinja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
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

# Build (15-30 minutes)
ninja -C build-shared-speed -j$(nproc)

# Install
sudo ninja -C build-shared-speed install
sudo ldconfig
```

**Result**:

- Build time: ~15-30 minutes
- Your app binaries: ~500-600 KB (vs 3.1 MB before optimization)
- Runtime: 30-60% faster than defaults

## Complete CMake Options Reference

### Core Build Configuration

| Option                 | Default        | Recommended    | Purpose                                        |
|------------------------|----------------|----------------|------------------------------------------------|
| `CMAKE_BUILD_TYPE`     | -              | `Release`      | Sets optimization level and defines            |
| `CMAKE_INSTALL_PREFIX` | `/usr/local`   | `/usr/local`   | Where to install libraries/headers             |
| `CMAKE_CXX_STANDARD`   | `14`           | `20`           | C++ standard version (match your project)      |
| `BUILD_SHARED_LIBS`    | `OFF`          | `ON`           | Build .so instead of .a (smaller app binaries) |
| `CMAKE_C_COMPILER`     | system default | `/usr/bin/gcc` | C compiler to use                              |
| `CMAKE_CXX_COMPILER`   | system default | `/usr/bin/g++` | C++ compiler to use                            |

### Optimization Flags

| Flag | Purpose | Recommended | Impact |
|------|---------|-------------|--------|
| `-O3` | Maximum optimization | **YES** | 10-20% faster, slightly larger code |
| `-march=native` | Use CPU-specific instructions | **YES** | 5-10% faster, not portable |
| `-mtune=native` | Tune for specific CPU | **YES** | Small improvement, not portable |
| `-DNDEBUG` | Remove assert() checks | **YES** | 2-5% faster, smaller code |
| `-g0` | No debug symbols | **YES** | Much smaller binaries, can't debug |

**Combined in CMake**:

```cmake
-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"
-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"
```

**Portability Note**: `-march=native -mtune=native` creates binaries optimized for your specific CPU. Remove these
flags if deploying to different CPU architectures.

### gRPC Language Plugin Options

**Only build what you need** - each plugin adds ~500 KB and build time.

| Option | Default | C++ Only | All Languages |
|--------|---------|----------|---------------|
| `gRPC_BUILD_GRPC_CPP_PLUGIN` | `ON` | `ON` | `ON` |
| `gRPC_BUILD_GRPC_CSHARP_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_NODE_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_PHP_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_PYTHON_PLUGIN` | `ON` | `OFF` | `ON` |
| `gRPC_BUILD_GRPC_RUBY_PLUGIN` | `ON` | `OFF` | `ON` |

**Savings**: Disabling 6 unused plugins saves ~3 MB build artifacts + faster build time.

### gRPC Core Options

| Option | Default | Recommended | Purpose |
|--------|---------|-------------|---------|
| `gRPC_INSTALL` | `ON` | `ON` | Install to CMAKE_INSTALL_PREFIX |
| `gRPC_BUILD_TESTS` | `OFF` | `OFF` | Don't build gRPC tests (saves time) |
| `gRPC_BUILD_CODEGEN` | `ON` | `ON` | Build protoc and grpc_cpp_plugin |
| `gRPC_BUILD_GRPCPP_OTEL_PLUGIN` | `OFF` | `OFF` | OpenTelemetry plugin (rarely needed) |
| `gRPC_DOWNLOAD_ARCHIVES` | `ON` | `ON` | Auto-download dependencies |

### Dependency Provider Options

**Provider types**:

- `module` = Build from gRPC's submodules (slower, self-contained)
- `package` = Use system-installed packages (faster, requires system libs)

| Dependency | Default | Recommended | Reason |
|------------|---------|-------------|--------|
| `gRPC_SSL_PROVIDER` | `module` | `package` | Use system OpenSSL (security updates) |
| `gRPC_ZLIB_PROVIDER` | `module` | `package` | System zlib already optimized |
| `gRPC_CARES_PROVIDER` | `module` | `module` | Build from gRPC submodule (version-matched) |
| `gRPC_RE2_PROVIDER` | `module` | `module` | Build from gRPC submodule (version-matched) |
| `gRPC_ABSL_PROVIDER` | `module` | `module` | **Must match Protobuf!** See note below |
| `gRPC_PROTOBUF_PROVIDER` | `module` | `module` | Build from submodule (version match!) |

**IMPORTANT - The gRPC Bundle Concept**:

- **gRPC, Protobuf, Abseil, c-ares, and RE2 are tightly coupled** and should all be built together from gRPC's
  submodules
- **Why `module` for all five**:
  - Version compatibility: gRPC bundles tested versions of all dependencies
  - Compiler consistency: All built with same flags to avoid template instantiation issues
  - Self-contained: No conflicts with potentially outdated or mismatched system versions
- **Only OpenSSL and zlib use `package`**: These are universal dependencies with stable ABIs

### Protobuf-Specific Options

| Option | Default | Recommended | Purpose |
|--------|---------|-------------|---------|
| `protobuf_BUILD_TESTS` | `ON` | `OFF` | Skip protobuf tests |
| `protobuf_BUILD_EXAMPLES` | `OFF` | `OFF` | Skip examples |
| `protobuf_BUILD_CONFORMANCE` | `OFF` | `OFF` | Skip conformance tests |
| `protobuf_BUILD_PROTOC_BINARIES` | `ON` | `ON` | Build protoc compiler |
| `protobuf_BUILD_LIBPROTOC` | `OFF` | `ON` | Build libprotoc (needed for codegen) |
| `protobuf_WITH_ZLIB` | varies | `ON` | Enable zlib compression |
| `protobuf_DISABLE_RTTI` | `OFF` | `OFF` | Keep RTTI (needed by some features) |
| `protobuf_ALLOW_CCACHE` | `OFF` | `ON` if ccache | Speed up rebuilds |

### Advanced/Optional Options

| Option | Default | When to Use | Impact |
|--------|---------|-------------|--------|
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION` | `OFF` | Maximum speed, have time | 5-15% faster, 2x build time |
| `CMAKE_C_COMPILER_LAUNCHER=ccache` | - | Have ccache installed | Much faster rebuilds |
| `CMAKE_CXX_COMPILER_LAUNCHER=ccache` | - | Have ccache installed | Much faster rebuilds |

**Link-Time Optimization (LTO)**:

```cmake
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
```

- **Pro**: faster runtime, better dead code elimination
- **Con**: Build takes 50%-150% longer
- **When**: Production builds where performance matters

**ccache**:

```cmake
-DCMAKE_C_COMPILER_LAUNCHER=ccache
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
-Dprotobuf_ALLOW_CCACHE=ON
```

- **Pro**: Dramatically faster rebuilds (minutes instead of hours)
- **Con**: Need to install ccache first: `sudo dnf install ccache`
- **When**: Active development with frequent rebuilds

## Build Scenarios

### Scenario 1: Maximum Speed (Recommended)

Documented in Quick Start section above.

**Result**:

- Build time: ~15-30 minutes
- Runtime: 30-60% faster than defaults
- Your app binaries: ~500-600 KB

### Scenario 2: Portable (Different CPU Architectures)

Remove CPU-specific flags from Quick Start:

```cmake
# Change from:
-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"
-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"

# To:
-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0"
-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g0"
```

**Result**:

- Slightly slower (5-10% loss) but works on any x86-64 CPU
- Good for distributing binaries

### Scenario 3: Debug Build (Development)

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

**Result**:

- Full debug symbols for debugging
- No optimizations (easier to step through)
- Larger binaries (~11 MB)

## Rebuild Your Project

After gRPC/Protobuf are installed:

```bash
cd /mnt/c/Users/ajcote/git/anderewrey/grpc_reactor_routeguide

# Clean previous build
rm -rf cmake-build-release-wsl-almalinux9-clang

# Configure
cmake -B cmake-build-release-wsl-almalinux9-clang \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -GNinja

# Build
cmake --build cmake-build-release-wsl-almalinux9-clang

# Check results
ls -lh cmake-build-release-wsl-almalinux9-clang/applications/*/route_guide_*
```

### Verify Shared Library Dependencies

```bash
cd cmake-build-release-wsl-almalinux9-clang
ldd applications/reactor/route_guide_active_reactor_client | grep -E "libprotobuf|libgrpc"
```

Should show:

```text
libgrpc++.so.1.73 => /usr/local/lib/libgrpc++.so.1.73
libprotobuf.so.31 => /usr/local/lib64/libprotobuf.so.31
libgrpc.so.48 => /usr/local/lib/libgrpc.so.48
```

## Binary Size Optimization

### Achieved Results

| Configuration | Binary Size | Reduction | Status |
|---------------|-------------|-----------|--------|
| Original Release | 3.1 MB | Baseline | — |
| **Phase 1** (symbol stripping + dead code) | **2.1 MB** | **32%** | Active |
| **Phase 2** (shared gRPC/Protobuf) | **500-600 KB** | **80-84%** | Active |

### Phase 1: CMake-Level Optimizations (Active)

**Implemented in CMakeLists.txt:148-156:**

```cmake
# Binary Size Optimizations for Release/MinSizeRel builds
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    # Enable function/data sections for dead code elimination
    add_compile_options(-ffunction-sections -fdata-sections)

    # Enable linker garbage collection and strip symbols
    add_link_options(-Wl,--gc-sections -s)

    message(STATUS "Binary size optimizations enabled")
endif()
```

**What this does:**

1. **Symbol stripping** (`-s`): Removes debug symbols → ~29% reduction
2. **Function sections** (`-ffunction-sections`): Each function in own section
3. **Dead code elimination** (`-Wl,--gc-sections`): Linker removes unreferenced sections → ~5-10% reduction

**Result**: 3.1 MB → 2.1 MB (32% reduction)

### Phase 2: Shared gRPC/Protobuf Bundle (Active)

**Key change**: Rebuild gRPC with `-DBUILD_SHARED_LIBS=ON` (see Quick Start section above)

**Before**: Protobuf was static (6.2 MB libprotobuf.a embedded in each binary)
**After**: All gRPC components are shared libraries

**Benefits:**

- Binaries: 2.1 MB → 500-600 KB (71% additional reduction)
- Memory: Single copy in RAM across all processes
- Updates: Can update libraries without recompiling app
- Deployment: Modern container-friendly approach

**Result**: 3.1 MB → 500-600 KB (80-84% total reduction)

### Library Composition Analysis

**Debug Build Example** (route_guide_active_reactor_client: 3.9 MB):

```text
Text section:    ~1.0 MB  (executable code)
  - Application code (debuggable): ~400 KB
  - spdlog (speed-optimized): ~300 KB
  - gflags (speed-optimized): ~50 KB
  - Other static libs: ~250 KB

Debug symbols:   ~2.8 MB  (application code only)
```

Shared libraries (not in binary, one copy in memory):

- gRPC + Protobuf + Abseil + c-ares + RE2: ~30 MB total
- OpenSSL + zlib: System libraries

### Why NOT Use `-Os` (Size Optimization)

This project prioritizes **speed over size**:

- `-Os` reduces performance by 5-10%
- `-O3` maintains maximum speed
- Shared libraries already give us small binaries (~600 KB)

**Current approach (recommended)**:

- Use `-O3 -march=native -mtune=native` for maximum speed
- Reduce size via proper linking (shared gRPC bundle)
- Result: Fast binaries at reasonable size

## Library Linking Strategy

### 1. Heavy Infrastructure (Shared Libraries)

**Libraries**: gRPC, Protobuf, Abseil, c-ares, RE2

**Strategy**: Build together as shared libraries from gRPC source

**Rationale**:

- **Size**: 6+ MB per binary if static
- **Version coupling**: Must match versions
- **Memory efficiency**: Single copy in RAM
- **Updates**: Can update without recompiling
- **Compiler consistency**: All built with same compiler

**Result**:

- Release binaries: ~500-600 KB each
- Debug binaries: ~3.6-4.3 MB each

### 2. Light Utilities (Static, Speed-Optimized)

**Libraries**: spdlog, gflags, glaze

**Strategy**: Static libraries, but **always compile with optimization**

**Special handling** (common/CMakeLists.txt:30-43):

```cmake
# Optimize third-party FetchContent dependencies even in Debug builds
target_compile_options(spdlog PRIVATE -O3 -DNDEBUG -march=native -mtune=native -g0)
target_compile_options(gflags_nothreads_static PRIVATE -O3 -DNDEBUG -march=native -mtune=native -g0)
```

**Rationale**:

- Small size: ~1.5 MB total when optimized
- Simple deployment: No .so files
- Fast performance: Same optimization as gRPC
- Your app code stays debuggable

**Result**:

- Debug builds: 55% smaller (8.5 MB → 3.6 MB)
- Release builds: Minimal impact

### 3. Project Libraries (Static, OBJECT)

**Libraries**: rg_proto, rg_service, protobuf_utils, common

**Strategy**: Static libraries, with rg_proto as OBJECT library

**Structure** (rg_service/CMakeLists.txt):

```cmake
add_library(rg_proto OBJECT
    ${PROTO_IMPORT_DIRS}/route_guide.proto)

add_library(rg_service
    rg_utils.cpp
    rg_db.cpp
    rg_logger.cpp
    route_guide_service.h
    rg_logger.h)
target_link_libraries(rg_service PUBLIC rg_proto protobuf_utils common)
```

**Rationale**:

- Prevents duplication: rg_proto included once as OBJECT library
- Build organization: Separate service code, generic utilities, and C++ compat code
- Link-time optimization: Better dead code elimination

**Result**:

- librg_service.a: ~15 MB (includes service logic, utils, db, logger)

### Summary Table

| Library Category | Type | Rationale |
|------------------|------|-----------|
| gRPC + Protobuf + Abseil + c-ares + RE2 | Shared | Heavy infrastructure bundle |
| OpenSSL + zlib | System | Security updates, universal |
| spdlog + gflags + glaze | Static (optimized) | Small utilities, speed-optimized |
| rg_proto + rg_service + protobuf_utils + common | Static | Project code, OBJECT lib prevents duplication |
| EventLoop | Shared | External library |

## Compiler Compatibility (Critical!)

### The Problem: Template Instantiation Incompatibility

**Rule**: gRPC, Protobuf, Abseil, and your application **must all use the same compiler** (all GCC or all Clang).

**Why**: These libraries are extremely template-heavy. Different compilers generate different symbols for the same
template code, causing linker errors like:

```text
undefined reference to `absl::lts_20250127::log_internal::LogMessage::operator<<(unsigned long)'
```

### Automatic Compiler Checking

**This project includes automatic checking** (CMakeLists.txt:25-124):

- Inspects `/usr/local/lib/libgrpc++.so`, `libprotobuf.so`, `libabsl_base.so`
- Extracts compiler info using `readelf -p .comment`
- Compares library compiler with your current compiler
- **Fatal error** if compiler type mismatch (GCC vs Clang)
- **Warning** if version mismatch (GCC 11 vs GCC 13)

### Manual Verification

Check which compiler built your libraries:

```bash
# Check gRPC
readelf -p .comment /usr/local/lib/libgrpc++.so | grep -i "gcc\|clang"

# Check Protobuf
readelf -p .comment /usr/local/lib64/libprotobuf.so | grep -i "gcc\|clang"

# Check Abseil
readelf -p .comment /usr/local/lib64/libabsl_base.so | grep -i "gcc\|clang"
```

**All three should show the SAME compiler version.**

### Solutions if Mismatch

#### Option 1: Match System Library Compiler

```bash
# Use compiler that matches system libraries
cmake -B build -DCMAKE_CXX_COMPILER=g++  # If libraries are GCC-built
# or
cmake -B build -DCMAKE_CXX_COMPILER=clang++  # If libraries are Clang-built
```

#### Option 2: Rebuild gRPC Stack

Rebuild gRPC, Protobuf, and Abseil with your preferred compiler (see Quick Start section).

### Bypass Check (NOT RECOMMENDED)

Only if you're certain about compatibility:

```bash
cmake -B build -DSKIP_COMPILER_CHECK=ON
```

## Deployment Considerations

### Shared Libraries to Deploy

When deploying, you need these shared libraries:

**From /usr/local/lib:**

- libgrpc++.so.1.73
- libgrpc.so.48
- libabsl_*.so (many Abseil components)
- libaddress_sorting.so
- libgpr.so
- libre2.so
- libupb.so

**From /usr/local/lib64:**

- libprotobuf.so.31
- libc-ares.so

**Deployment Methods:**

1. **System Installation** (current setup):

   ```bash
   sudo ninja install  # Installs to /usr/local
   sudo ldconfig       # Updates library cache
   ```

2. **LD_LIBRARY_PATH**:

   ```bash
   export LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64:$LD_LIBRARY_PATH
   ```

3. **RPATH in Binary** (set during CMake):

   ```cmake
   set(CMAKE_BUILD_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")
   set(CMAKE_INSTALL_RPATH "${GRPC_PREFIX}/lib64;${GRPC_PREFIX}/lib")
   ```

   Already configured in CMakeLists.txt:132-141

### Docker/Container Deployment

```dockerfile
FROM almalinux:9

# Install runtime dependencies
RUN dnf install -y openssl-libs zlib

# Copy shared libraries from build container
COPY --from=builder /usr/local/lib/libgrpc*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libabsl*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib64/libprotobuf*.so* /usr/local/lib64/
COPY --from=builder /usr/local/lib64/libc-ares*.so* /usr/local/lib64/

# Update library cache
RUN ldconfig

# Copy your binaries
COPY --from=builder /app/build/applications/*/* /app/

CMD ["/app/route_guide_callback_server"]
```

## Alternative: System Package Manager

**WARNING**: When using system packages, you **lose the gRPC bundle benefits**:

- **No version guarantees**: System gRPC, Protobuf, Abseil, c-ares, and RE2 may not be version-matched
- **Unknown compiler**: System packages may be built with different compiler than your application
- **Unknown optimizations**: You don't control the optimization flags used
- **Potential conflicts**: System packages may have mismatched dependencies

**For production or performance-critical applications, building from source (as documented above) is strongly
recommended.**

### Ubuntu 24.04+

```bash
sudo apt update
sudo apt install -y \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libabsl-dev \
    libc-ares-dev \
    libre2-dev
```

### AlmaLinux 9 / RHEL 9

```bash
# Enable EPEL and CodeReady repos
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb

# Install gRPC and all dependencies
sudo dnf install -y \
    grpc-devel \
    grpc-plugins \
    protobuf-devel \
    abseil-cpp-devel \
    c-ares-devel \
    re2-devel
```

## Troubleshooting

### CMake Errors

**Error**: `Could not find package OpenSSL` or `Could not find package ZLIB`

```bash
# AlmaLinux/RHEL
sudo dnf install -y openssl-devel zlib-devel

# Ubuntu/Debian
sudo apt install -y libssl-dev zlib1g-dev
```

**Note**: If you see errors about missing c-ares, RE2, Abseil, or Protobuf packages, ensure you're using
`gRPC_*_PROVIDER=module` for these dependencies as documented above. They should **NOT** be installed from system
packages.

### Build Errors

**Error**: `ninja: build stopped: subcommand failed`

Check the actual error above this message. Common issues:

- Out of memory: Reduce parallel jobs `ninja -j4` instead of `-j$(nproc)`
- Compiler version: GCC 11+ or Clang 14+ required for C++20

**Error**: LTO/IPO errors

Disable LTO:

```cmake
# Remove this flag
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
```

### Runtime Errors

**Error**: `libprotobuf.so not found` at runtime or `grpc_cpp_plugin: error while loading shared libraries`

Update library cache:

```bash
# Create ldconfig configuration
echo -e "/usr/local/lib\n/usr/local/lib64" | sudo tee /etc/ld.so.conf.d/usr-local.conf

# Update library cache
sudo ldconfig

# Verify fix
ldd /usr/local/bin/grpc_cpp_plugin | grep "not found"  # Should return nothing
ldconfig -p | grep libprotobuf
```

**Error**: Version mismatch

Ensure gRPC and Protobuf were built together. Rebuild both:

```bash
cd ~/git/grpc/build-shared-speed
ninja clean
ninja -j$(nproc)
sudo ninja install
sudo ldconfig
```

### Compiler Mismatch Errors

See "Compiler Compatibility" section above. CMakeLists.txt will detect and prevent mismatches automatically.

## Multi-Compiler Setup

If you need multiple compilers (GCC 11, GCC 13, Clang), install to separate prefixes:

### Compiler-Specific Prefixes

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

### Update ldconfig for all prefixes

```bash
echo "/usr/local/gcc11/lib
/usr/local/gcc11/lib64" | sudo tee /etc/ld.so.conf.d/gcc11-local.conf

echo "/usr/local/gcc13/lib
/usr/local/gcc13/lib64" | sudo tee /etc/ld.so.conf.d/gcc13-local.conf

echo "/usr/local/clang/lib
/usr/local/clang/lib64" | sudo tee /etc/ld.so.conf.d/clang-local.conf

sudo ldconfig
```

## Performance Characteristics

### Speed Optimizations Applied

| Optimization | Impact | Trade-off |
|--------------|--------|-----------|
| `-O3` | Maximum speed (inlining, vectorization) | Larger code (mitigated by shared libs) |
| `-march=native -mtune=native` | CPU-specific instructions (AVX, AVX2) | Not portable to different CPUs |
| LTO (`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`) | Cross-module optimization | Slower build time (~2x) |
| Shared libraries | Slightly faster startup | Need to deploy .so files |
| Static utilities (optimized) | No dynamic linking overhead | None (small size) |

### Runtime Performance Improvement

| Optimization | Improvement | Notes |
|--------------|-------------|-------|
| `-O3` vs `-O0` | +20-40% | Biggest impact |
| `-march=native -mtune=native` | +5-15% | CPU-specific instructions |
| LTO | +5-15% | Cross-module inlining |
| **Combined** | **+30-60%** | Varies by workload |

**Note**: Actual performance depends on your specific workload. Run benchmarks to measure.

### Portability vs Speed

**Maximum Speed** (This Build):

```text
-O3 -march=native -mtune=native + LTO
```

- Optimized for your specific CPU
- Only works on your CPU architecture

**Portable Speed**:

```text
-O3 + LTO  (remove -march=native -mtune=native)
```

- Works on any x86-64 CPU
- Still highly optimized

## Verification

### Check Installation

```bash
# Verify shared libraries
ls -lh /usr/local/lib/libgrpc++.so*
ls -lh /usr/local/lib64/libprotobuf.so*

# Check versions
/usr/local/bin/protoc --version
/usr/local/bin/grpc_cpp_plugin --version

# Verify library cache
ldconfig -p | grep -E "libgrpc|libprotobuf|libabsl"
```

Expected output:

```text
/usr/local/lib/libgrpc++.so -> libgrpc++.so.1.73
/usr/local/lib/libgrpc++.so.1.73 -> libgrpc++.so.1.73.1
/usr/local/lib64/libprotobuf.so -> libprotobuf.so.31.0.0

libprotoc 31.0
```

### Test Your Application

```bash
cd /your/project
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja
ninja -C build

# Check binary size
ls -lh build/your_binary

# Verify it links to correct libraries
ldd build/your_binary | grep -E "libgrpc|libprotobuf"
```

Should show:

```text
libgrpc++.so.1.73 => /usr/local/lib/libgrpc++.so.1.73
libprotobuf.so.31 => /usr/local/lib64/libprotobuf.so.31
libgrpc.so.48 => /usr/local/lib/libgrpc.so.48
```

### Monitoring Binary Size

```bash
# Section sizes
size <binary>

# Largest symbols (if not stripped)
nm --print-size --size-sort --radix=d <binary> | tail -50

# Library dependencies
ldd <binary>

# Compare builds
ls -lh <binary1> <binary2>
```

### Verify Optimization

```bash
# Check shared library dependencies
ldd applications/reactor/route_guide_active_reactor_client | grep -E "libprotobuf|libgrpc"

# Check static library sizes
ls -lh cmake-build-release-wsl-almalinux9-clang/rg_service/librg_service.a

# Verify symbols are stripped
file applications/reactor/route_guide_active_reactor_client
# Should show: "stripped" in output
```

## Expected Results

### Build Times

| Configuration | Time | Reason |
|---------------|------|--------|
| Default (all plugins, all providers=module) | 60-90 min | Builds everything from source |
| Recommended (C++ only, system OpenSSL/zlib) | 15-30 min | Reuses system libs, skips unused plugins |
| With LTO | 30-60 min | Cross-module optimization takes time |
| With ccache (rebuilds) | 2-5 min | Cached compilation units |

### Binary Sizes (Your Application)

| Configuration | Binary Size | Explanation |
|---------------|-------------|-------------|
| Static protobuf, no optimization | ~3.1 MB | Baseline |
| Static protobuf, optimized (-O3, strip) | ~2.1 MB | Phase 1 optimization |
| **Shared protobuf, optimized** | **~500-600 KB** | **This build** |
| Shared + LTO | ~500-600 KB | LTO doesn't affect binary size much |

## Comparison Table

| Configuration | Binary Size | Runtime Perf | Debug Info | Status |
|---------------|-------------|--------------|------------|--------|
| Debug (old) | 11 MB | Slow | Full | Baseline |
| Debug (optimized deps) | **3.7-4.3 MB** | Medium | App only | **Active** |
| Release (old) | 3.1 MB | Fast | Symbols | Baseline |
| Release (Phase 1) | 2.1 MB | Faster | None | Achieved |
| **Release (Phase 2)** | **500-600 KB** | **Fastest** | None | **Active** |

## Summary

**Key Principles:**

1. **Speed First**: Always use `-O3 -march=native -mtune=native`, never `-Os`
2. **Eliminate Duplication**: Shared libs for heavy infrastructure (gRPC bundle)
3. **Smart Linking**: Static libs for light utilities with optimization
4. **Compiler Consistency**: All gRPC components built with same compiler

**Achieved Results:**

- **Release builds**: 3.1 MB → 500-600 KB (80-84% reduction)
- **Debug builds**: 11 MB → 3.7-4.3 MB (58-66% reduction)
- **Fastest possible runtime performance**: 30-60% improvement

**Quick Reference Commands:**

```cmake
# Always Include (Core recommendations)
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"
-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -mtune=native -g0"
-DBUILD_SHARED_LIBS=ON
-DgRPC_BUILD_GRPC_CPP_PLUGIN=ON
-DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF
-DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF
-DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF
-DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF
-DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF
-DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF
-DgRPC_BUILD_TESTS=OFF
-DgRPC_SSL_PROVIDER=package
-DgRPC_ZLIB_PROVIDER=package
-DgRPC_CARES_PROVIDER=module
-DgRPC_RE2_PROVIDER=module
-DgRPC_ABSL_PROVIDER=module
-DgRPC_PROTOBUF_PROVIDER=module
-Dprotobuf_BUILD_TESTS=OFF

# Optional (Add if needed)
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON  # For maximum speed (2x build time)
-DCMAKE_C_COMPILER_LAUNCHER=ccache       # For faster rebuilds (requires ccache)
-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
-Dprotobuf_ALLOW_CCACHE=ON
```

**Next Steps:**

- For reactor pattern documentation, see [reactor_client.md](/applications/reactor/reactor_client.md)
- For multi-compiler setup, see "Multi-Compiler Setup" section above
