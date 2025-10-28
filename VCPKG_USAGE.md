# vcpkg Integration Guide

This document describes the vcpkg-based dependency management system for the gRPC Reactor RouteGuide project.

## Overview

The project uses vcpkg to manage gRPC, Protobuf, and Abseil dependencies with the following goals:

- Multi-compiler support (GCC 11, GCC 13, Clang 19)
- Release-only dependency builds for optimal performance
- Linux ABI compatibility for mixing Debug application builds with Release libraries
- Custom optimization flags and build configurations

## Project Structure

```
grpc_reactor_routeguide/
├── vcpkg.json                      # Dependency manifest
├── vcpkg-configuration.json        # Registry and overlay configuration
├── CMakePresets.json               # CLion/CMake presets for easy configuration
├── vcpkg/
│   ├── triplets/                   # Custom compiler-specific triplets
│   │   ├── x64-linux-gcc-release.cmake     # System GCC (11.5.0 on AlmaLinux 9)
│   │   ├── x64-linux-gcc11-release.cmake   # GCC 11 (requires gcc-11 binary)
│   │   ├── x64-linux-gcc13-release.cmake   # GCC 13 from Red Hat toolset
│   │   └── x64-linux-clang19-release.cmake # Clang 19
│   ├── toolchains/
│   │   └── ccache-toolchain.cmake  # ccache integration
│   └── ports/
│       └── grpc/                   # Custom gRPC port overlay
│           ├── portfile.cmake      # Build script with submodule support
│           └── vcpkg.json          # Port manifest
└── vcpkg_installed/                # Generated: installed packages (gitignored)
```

## Verified Multi-Compiler Support

This project has been successfully tested with:
- **Clang 19.1.7** (AlmaLinux 9 system package)
- **GCC 11.5.0** (AlmaLinux 9 default)
- **GCC 13.3.1** (Red Hat toolset)

All combinations successfully:
- Build vcpkg dependencies with Release optimization (`-O3 -march=native -mtune=native`)
- Build Debug application linking against Release libraries (Linux ABI compatibility)
- Link executables without undefined reference errors
- Produce working binaries (7-8 MB Debug builds)

## Quick Start

### Prerequisites

1. vcpkg installed on your system (set `VCPKG_ROOT` environment variable to your vcpkg installation)
2. Compilers: GCC 11, GCC 13, or Clang 19
3. System packages: OpenSSL, zlib (for vcpkg builds)
4. ccache (optional but recommended for faster rebuilds)
5. Ninja build system (optional, recommended for parallel builds)

### Install Dependencies

```bash
# Install gRPC and dependencies for Clang 19
vcpkg install --triplet=x64-linux-clang19-release

# Or for GCC 11
vcpkg install --triplet=x64-linux-gcc11-release

# Or for GCC 13
vcpkg install --triplet=x64-linux-gcc13-release
```

This installs to `vcpkg_installed/` in the project root (gitignored).

### Build Application

#### Clang 19 (Recommended)

```bash
# Configure with Clang 19 + ccache (Debug build with Release libraries)
cmake -B cmake-build-vcpkg-debug-clang \
  -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-clang19-release \
  -DVCPKG_OVERLAY_TRIPLETS=./vcpkg/triplets \
  -DVCPKG_OVERLAY_PORTS=./vcpkg/ports

# Build (all 37 targets)
cmake --build cmake-build-vcpkg-debug-clang
```

**Explanation of flags:**
- `-GNinja`: Use Ninja generator for faster parallel builds
- `-DCMAKE_BUILD_TYPE=Debug`: Build application in Debug mode (enables debugging symbols, no optimization)
- `-DCMAKE_C_COMPILER_LAUNCHER=ccache`: Use ccache to cache C compilation results (speeds up rebuilds)
- `-DCMAKE_C_COMPILER=clang`: Use Clang as the C compiler
- `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`: Use ccache to cache C++ compilation results
- `-DCMAKE_CXX_COMPILER=clang++`: Use Clang++ as the C++ compiler
- `-DCMAKE_CXX_STANDARD=20`: Use C++20 standard
- `-DCMAKE_TOOLCHAIN_FILE=...`: Path to vcpkg's CMake toolchain file (enables vcpkg integration)
- `-DVCPKG_TARGET_TRIPLET=x64-linux-clang19-release`: Use Clang 19-compiled Release libraries
- `-DVCPKG_OVERLAY_TRIPLETS=./vcpkg/triplets`: Path to custom triplet definitions
- `-DVCPKG_OVERLAY_PORTS=./vcpkg/ports`: Path to custom port overlays (for gRPC submodule support)

**Why this works:**
- Linux ABI compatibility allows mixing Debug application with Release libraries
- Application gets full debug info for development
- Dependencies (gRPC/Protobuf/Abseil) are optimized with `-O3 -march=native -mtune=native`
- ccache dramatically speeds up rebuilds (especially useful during development)

#### GCC 11

```bash
# Configure with GCC 11 + ccache
cmake -B cmake-build-vcpkg-debug-gcc \
  -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-gcc-release \
  -DVCPKG_OVERLAY_TRIPLETS=./vcpkg/triplets \
  -DVCPKG_OVERLAY_PORTS=./vcpkg/ports

# Build
cmake --build cmake-build-vcpkg-debug-gcc
```

**Note:** The `x64-linux-gcc-release` triplet uses the system default GCC (version 11.5.0 on AlmaLinux 9).
Replace `gcc`/`g++` with `gcc-11`/`g++-11` if you have version-specific compiler binaries installed.

#### GCC 13 (from Red Hat toolset)

```bash
# Configure with GCC 13 from toolset
cmake -B cmake-build-vcpkg-debug-gcc13 \
  -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_COMPILER=/opt/rh/gcc-toolset-13/root/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER=/opt/rh/gcc-toolset-13/root/usr/bin/g++ \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-gcc13-release \
  -DVCPKG_OVERLAY_TRIPLETS=./vcpkg/triplets \
  -DVCPKG_OVERLAY_PORTS=./vcpkg/ports

# Build
cmake --build cmake-build-vcpkg-debug-gcc13
```

**Note:** Specify full path to GCC 13 from the Red Hat toolset. The triplet expects `gcc-13`/`g++-13` binaries.

### Alternative: Install to Build Directory

To keep all vcpkg artifacts inside your build directory:

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/home/foo/git/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR=build/vcpkg_installed \
  -DVCPKG_TARGET_TRIPLET=x64-linux-clang19-release \
  -DVCPKG_OVERLAY_TRIPLETS=./vcpkg/triplets \
  -DVCPKG_OVERLAY_PORTS=./vcpkg/ports

cmake --build build
```

This approach keeps everything under `build/` which is already gitignored.

## Custom Triplets

### Why Custom Triplets?

1. **Release-only builds**: `VCPKG_BUILD_TYPE=release` builds dependencies once in Release mode
2. **Compiler-specific**: Each triplet specifies exact compiler to ensure ABI compatibility
3. **Optimization flags**: Custom flags for aggressive optimization
4. **ccache integration**: Speeds up rebuilds via chainloaded toolchain

### Triplet Configuration

All triplets share the same structure:

```cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Build only Release configuration
set(VCPKG_BUILD_TYPE release)

# Chainload toolchain with ccache
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../toolchains/ccache-toolchain.cmake")

# Compiler-specific settings with optimizations
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_C_COMPILER=clang-19"
    "-DCMAKE_CXX_COMPILER=clang++-19"
    "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -march=native -mtune=native -g0"
    "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -march=native -mtune=native -g0"
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON")
```

The compiler paths differ for each triplet:
- `x64-linux-gcc11-release.cmake`: Uses `gcc-11` and `g++-11`
- `x64-linux-gcc13-release.cmake`: Uses `gcc-13` and `g++-13`
- `x64-linux-clang19-release.cmake`: Uses `clang-19` and `clang++-19`

## Custom gRPC Port

### Why a Custom Port?

The standard vcpkg gRPC port does not support the requirements:

1. **Git submodules required**: gRPC needs Protobuf, Abseil, c-ares, RE2 from submodules for version consistency
2. **vcpkg functions don't support submodules**:
   - `vcpkg_from_github`: Downloads release tarballs (no submodules)
   - `vcpkg_from_git`: Uses `git archive` internally (excludes submodules)
3. **Solution**: Direct `git clone --recurse-submodules` (official vcpkg approach per issues #6886, #1036)

### Port Implementation

The custom port at `vcpkg/ports/grpc/portfile.cmake` implements:

```cmake
# Shallow clone with submodules
if(NOT EXISTS "${SOURCE_PATH}/.git")
    vcpkg_execute_required_process(
        COMMAND ${GIT} clone
            --depth 1
            --branch v1.73.1
            --recurse-submodules
            https://github.com/grpc/grpc.git
            ${SOURCE_PATH}
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}/src"
        LOGNAME git-clone-grpc
    )
else()
    message(STATUS "Using cached gRPC source at ${SOURCE_PATH}")
endif()
```

Key configuration options:
- gRPC v1.73.1 (June 2025, stable)
- Module providers: Protobuf, Abseil, c-ares, RE2 from submodules
- Package providers: OpenSSL, zlib from system (via vcpkg)
- C++ only (all other language plugins disabled)
- Optimization flags applied via triplet configuration

## Linux ABI Compatibility

### Key Insight

On Linux (unlike Windows/MSVC), mixing Debug and Release builds is safe and recommended:

```cpp
// Your application compiled with:
// -O0 -g (Debug, unoptimized, full symbols)

// Links to gRPC/Protobuf compiled with:
// -O3 -DNDEBUG (Release, fully optimized)

// Result on Linux: Works perfectly!
// Same C++ ABI, no runtime conflicts
```

**Why it works:**
- "Debug" and "Release" on Linux are just compiler flags
- No separate debug/release C++ standard library
- No ABI differences in STL containers or allocators
- Same compiler + same stdlib version = compatible binaries

### Benefits

| Build Type | Your Code | Dependencies | Result |
|------------|-----------|--------------|--------|
| **Debug** | `-O0 -g` | `-O3` (Release) | Fast debugging + fast execution |
| **Release** | `-O3` | `-O3` (Release) | Full optimization everywhere |
| **RelWithDebInfo** | `-O2 -g` | `-O3` (Release) | Balanced performance + debugging |

All three build types link to the **same gRPC binaries** (built once in Release mode).

## vcpkg Configuration Files

### vcpkg.json (Manifest)

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "grpc-reactor-routeguide",
  "version": "0.1.0",
  "dependencies": [
    "grpc"
  ]
}
```

Declares project dependencies. vcpkg automatically resolves transitive dependencies (OpenSSL, zlib, etc.).

### vcpkg-configuration.json

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg-configuration.schema.json",
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "4334d8b4c8916018600212ab4dd4bbdc343065d1"
  },
  "overlay-triplets": [
    "./vcpkg/triplets"
  ],
  "overlay-ports": [
    "./vcpkg/ports"
  ]
}
```

**Key settings:**
- **baseline**: Pinned to September 2025 release to avoid vcpkg-cmake REMOVE_DUPLICATES bug in later versions
- **overlay-triplets**: Use custom compiler-specific triplets
- **overlay-ports**: Use custom gRPC port with submodule support

## Troubleshooting

### Issue: Operation REMOVE_DUPLICATES not recognized

**Error:**
```
CMake Error at scripts/cmake/vcpkg_host_path_list.cmake:60 (message):
  Operation REMOVE_DUPLICATES not recognized.
```

**Cause:** vcpkg-cmake version 2025-08-07 or later has a regression.

**Solution:** Use September 2025 baseline (already configured in vcpkg-configuration.json):
```json
"baseline": "4334d8b4c8916018600212ab4dd4bbdc343065d1"
```

### Issue: Git submodules not found during build

**Error:**
```
CMake Error: add_subdirectory given source "third_party/cares/cares" which is not a directory.
```

**Cause:** Standard vcpkg functions don't support git submodules.

**Solution:** Use custom gRPC port overlay (already configured).

The custom port at `vcpkg/ports/grpc/portfile.cmake` uses direct git clone with `--recurse-submodules`.

### Issue: Mixing compilers causes undefined references

**Error:**
```
undefined reference to `absl::lts_20250127::log_internal::LogMessage::operator<<(unsigned long)'
```

**Cause:** gRPC, Protobuf, and Abseil are template-heavy and must all be compiled with the **same compiler**.

**Solution:** Always use matching triplet for your application compiler:

```bash
# Building with Clang 19
cmake -B build \
  -DCMAKE_CXX_COMPILER=clang++-19 \
  -DVCPKG_TARGET_TRIPLET=x64-linux-clang19-release  # Match!

# Building with GCC 11
cmake -B build \
  -DCMAKE_CXX_COMPILER=g++-11 \
  -DVCPKG_TARGET_TRIPLET=x64-linux-gcc11-release    # Match!
```

### Issue: Build artifacts in project root

By default, vcpkg installs packages to `vcpkg_installed/` in project root.

**Solution 1:** Already gitignored via `.gitignore` pattern `vcpkg_*/`

**Solution 2:** Install to build directory instead:
```bash
cmake -B build \
  -DVCPKG_INSTALLED_DIR=build/vcpkg_installed \
  ...
```

This keeps everything under `build/` which is already gitignored.

## Build Metrics

### vcpkg Dependency Installation (First Time)

**Clang 19:**
- **Total time:** 13.5 minutes
  - zlib: 6.9 seconds
  - openssl: 52 seconds
  - grpc: 12 minutes
- **Packages installed:** 6 (vcpkg-cmake, zlib, openssl, vcpkg-cmake-config, vcpkg-cmake-get-vars, grpc)
- **gRPC components built:**
  - gRPC v1.73.1
  - Protobuf v31.0.0 (from submodule)
  - Abseil ~60 libraries (from submodule)
  - c-ares (async DNS, from submodule)
  - RE2 (regex library, from submodule)
  - UPB (micro protobuf, from submodule)

**GCC 11:**
- **Total time:** 13 minutes
  - zlib: 6.9 seconds
  - openssl: 52 seconds
  - grpc: 12 minutes

### Application Build Time

**With vcpkg-installed dependencies:**
- **Configure:** ~1 minute (813 seconds total, includes vcpkg manifest resolution)
- **Build:** ~1 minute (37 targets with ccache)
- **Total:** ~2 minutes from clean configuration to executable

**Subsequent builds:**
- **Configure:** ~5 seconds (cached vcpkg dependencies)
- **Build:** ~10 seconds (ccache hits for unchanged files)

### Disk Space

- **Per triplet:** ~100 MB (Release-only builds)
- **All three triplets:** ~300 MB total (gcc-release, gcc13-release, clang19-release)
- **Comparison:** Standard Debug+Release builds would use ~200 MB per triplet

## Advanced Topics

### Switching Compilers

Each compiler needs its own vcpkg installation:

```bash
# Install for all three compilers
vcpkg install --triplet=x64-linux-gcc11-release
vcpkg install --triplet=x64-linux-gcc13-release
vcpkg install --triplet=x64-linux-clang19-release

# Switch between them in CMake
cmake -B build-gcc11 -DCMAKE_CXX_COMPILER=g++-11 \
  -DVCPKG_TARGET_TRIPLET=x64-linux-gcc11-release ...

cmake -B build-clang19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DVCPKG_TARGET_TRIPLET=x64-linux-clang19-release ...
```

### Updating gRPC Version

To update to a newer gRPC version:

1. Edit `vcpkg/ports/grpc/portfile.cmake`:
   ```cmake
   --branch v1.76.0  # Update version tag
   ```

2. Edit `vcpkg/ports/grpc/vcpkg.json`:
   ```json
   "version": "1.76.0"  # Update version
   ```

3. Rebuild:
   ```bash
   rm -rf vcpkg_installed
   vcpkg install --triplet=x64-linux-clang19-release
   ```

### Binary Caching

vcpkg supports binary caching to speed up builds across machines or CI/CD:

```bash
# Enable filesystem binary cache
export VCPKG_BINARY_SOURCES="clear;files,/path/to/cache,readwrite"

# Or use GitHub Packages for team sharing
export VCPKG_BINARY_SOURCES="clear;nuget,https://github.com/owner/repo,readwrite"
```

See vcpkg documentation for details.

### Static Linking

To build static libraries instead of shared:

1. Edit triplet file:
   ```cmake
   set(VCPKG_LIBRARY_LINKAGE static)  # Change from dynamic
   ```

2. Rebuild dependencies:
   ```bash
   rm -rf vcpkg_installed
   vcpkg install --triplet=x64-linux-clang19-release
   ```

**Note:** Static linking produces larger executables but eliminates runtime library dependencies.

## References

- vcpkg documentation: https://learn.microsoft.com/en-us/vcpkg/
- vcpkg triplet reference: https://learn.microsoft.com/en-us/vcpkg/users/triplets
- vcpkg binary caching: https://learn.microsoft.com/en-us/vcpkg/consume/binary-caching
- gRPC GitHub (submodule issues): https://github.com/microsoft/vcpkg/issues/6886
- Project-specific build guide: [GRPC_BUILD_GUIDE.md](GRPC_BUILD_GUIDE.md)
