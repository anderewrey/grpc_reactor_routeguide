# Developing

This document covers setting up a development environment, building the applications, running the binaries, running
tests, and linting. Dependencies are managed by [vcpkg][vcpkg], which fetches and builds all C++ libraries from
source. See [vcpkg-usage.md](/docs/vcpkg-usage.md) for multi-compiler details and [grpc-build-guide.md](/docs/grpc-build-guide.md)
for gRPC build tuning.

## Prerequisites

The build requires a C++20 compiler, CMake, Ninja, and Git. vcpkg supplies the C++ libraries (gRPC, Protobuf, Abseil,
spdlog, gflags, glaze, EventLoop, GoogleTest).

| Tool | Purpose | Minimum |
| ---- | ------- | ------- |
| C++ compiler | Build the applications (GCC or Clang) | C++20 support |
| CMake | Configure and drive the build | 3.25 |
| Ninja | Build generator used by all presets | recent |
| Git | Clone the repository and vcpkg | recent |
| Node.js / npx | Run markdownlint on documentation | recent |

### Packages to install

Install these with the system package manager. Package names vary by distribution; common names are given.

- C++ compiler: `gcc-c++` (or `g++`); use `clang` for a Clang build
- CMake: `cmake`
- Ninja: `ninja-build` (or `ninja`)
- Git: `git`
- Node.js providing `npx`: `nodejs` (used only to run markdownlint)

### From-source build dependencies (OpenSSL)

vcpkg builds OpenSSL (a gRPC dependency) from source, and OpenSSL's `Configure` is a Perl script that relies on
several core Perl modules. On distributions that ship a minimal Perl, these are packaged separately and must be
installed:

- `IPC::Cmd`: `perl-IPC-Cmd`
- `File::Copy`: `perl-File-Copy`
- `File::Compare`: `perl-File-Compare`
- `FindBin`: `perl-FindBin`

Some distributions bundle these modules in the base `perl` package instead.

OpenSSL also requires the Linux kernel headers (`kernel-headers`, also named `linux-libc-dev` or `linux-headers`).
These are normally already present, pulled in transitively by the C++ compiler's C library development package
(`glibc-devel` or equivalent); install explicitly only if a build complains.

### Optional: ccache

ccache speeds up rebuilds and is not required. When ccache is installed, the build uses it automatically: vcpkg
dependency builds pick it up via `vcpkg/toolchains/ccache-toolchain.cmake`, and the project's own targets via
`cmake/CompilerLauncher.cmake`. When ccache is absent, both are a no-op.

To force a specific launcher or override the auto-detection, set it explicitly before the first configure; CMake
reads it when it creates the build tree:

```bash
export CMAKE_C_COMPILER_LAUNCHER=ccache
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
```

### Optional: pre-commit

pre-commit runs the lint hooks (markdownlint, cpplint) and requires Python with pip. Without it, run the linters
directly as shown in [Lint and format](#lint-and-format).

## Get vcpkg

The build auto-detects vcpkg at `~/git/vcpkg` (see [VcpkgAutoDetect.cmake](/cmake/VcpkgAutoDetect.cmake) for the full
search list). Clone and bootstrap it there.

```bash
git clone https://github.com/microsoft/vcpkg ~/git/vcpkg
~/git/vcpkg/bootstrap-vcpkg.sh
```

To use an existing installation in a different location, set `VCPKG_ROOT`.

```bash
export VCPKG_ROOT=/path/to/vcpkg
```

## Configure and build

Configure with a preset, then build. The first configure triggers vcpkg to build gRPC and its dependencies from
source, which takes 15-30 minutes. Later configures reuse the cached libraries.

```bash
cmake --preset vcpkg-gcc-debug
cmake --build cmake-build-vcpkg-debug-gcc
```

Available presets:

| Preset | Compiler | Build type | Binary directory |
| ------ | -------- | ---------- | ---------------- |
| `vcpkg-gcc-debug` | GCC | Debug | `cmake-build-vcpkg-debug-gcc` |
| `vcpkg-gcc-release` | GCC | Release | `cmake-build-vcpkg-release-gcc` |
| `vcpkg-clang-debug` | Clang | Debug | `cmake-build-vcpkg-debug-clang` |
| `vcpkg-clang-release` | Clang | Release | `cmake-build-vcpkg-release-clang` |
| `vcpkg-gcc13-debug` | GCC 13 toolset | Debug | `cmake-build-vcpkg-debug-gcc13` |
| `vcpkg-gcc13-release` | GCC 13 toolset | Release | `cmake-build-vcpkg-release-gcc13` |

The `gcc13` presets expect the compiler at `/opt/rh/gcc-toolset-13`. Debug presets build the application in Debug
against Release libraries, which is supported on Linux (see [vcpkg-usage.md](/docs/vcpkg-usage.md)).

## Clean

There are two levels of build artifacts. The project build directories are gitignored (`cmake-*/`, `build*/`).

```bash
# Clear one preset's build directory (also removes its vcpkg_installed/)
rm -rf cmake-build-vcpkg-debug-gcc

# Clear all preset build directories
rm -rf cmake-build-*/
```

Removing a build directory keeps vcpkg's compiled dependencies, so the next configure restores them from cache
without recompiling gRPC. For a full clear that forces a from-source rebuild of all dependencies (~15-30 min on the
next configure), also remove vcpkg's global build artifacts:

```bash
rm -rf cmake-build-*/ ~/git/vcpkg/buildtrees ~/git/vcpkg/packages ~/git/vcpkg/vcpkg-running.lock
```

This leaves vcpkg's downloaded source tarballs (`~/git/vcpkg/downloads`) and binary cache in place. To also discard
those and force re-download, clear `~/git/vcpkg/downloads` and the binary cache at `~/.cache/vcpkg`.

vcpkg has no standalone clean command; deleting `buildtrees/` and `packages/` is the supported way to reclaim that
space. To avoid the accumulation in the first place, vcpkg can clean `buildtrees/` after each dependency builds. Pass
the option through `VCPKG_INSTALL_OPTIONS` in a preset's `cacheVariables` (downloads stay cached, so nothing
re-downloads):

```json
"VCPKG_INSTALL_OPTIONS": "--clean-buildtrees-after-build"
```

## Run

The binaries are placed under the preset's binary directory. The following uses `vcpkg-gcc-debug`.

```bash
DIR=cmake-build-vcpkg-debug-gcc
./$DIR/applications/blocking/route_guide_sync_server
./$DIR/applications/blocking/route_guide_sync_client
./$DIR/applications/callback/route_guide_callback_server
./$DIR/applications/callback/route_guide_callback_client
./$DIR/applications/reactor/route_guide_active_reactor_client
```

The client applications connect to a running server, so start a server before its matching client.

## Test

Tests use GoogleTest and run through CTest. See [testing.md](/docs/testing.md) for the test
architecture and coverage matrix.

```bash
ctest --test-dir cmake-build-vcpkg-debug-gcc --output-on-failure
```

## Lint and format

Run these before committing.

```bash
# Format C++ (Google style, 120 columns)
clang-format -i path/to/file.cpp

# Lint a markdown file
npx markdownlint-cli --config markdownlint.yaml "path/to/file.md"

# Run all hooks on git-tracked files (requires pre-commit)
pre-commit run --all-files
```

## Troubleshooting

- First-time dependency builds download sources from GitHub. A transient DNS failure can abort one download with
  `curl operation failed with error code 6 (Could not resolve hostname)`. vcpkg reports this error as non-retryable.
  The failure is usually transient. Re-run the configure; vcpkg resumes from the cached, already-built packages.
- An OpenSSL build failure mentioning `IPC::Cmd` (or another Perl module) means the Perl modules listed in
  [From-source build dependencies](#from-source-build-dependencies-openssl) are not installed.
- See [vcpkg-usage.md](/docs/vcpkg-usage.md) for compiler-mismatch and other vcpkg-specific issues.

## See also

- [vcpkg-usage.md](/docs/vcpkg-usage.md): vcpkg setup, custom triplets, multi-compiler support
- [grpc-build-guide.md](/docs/grpc-build-guide.md): gRPC build options and optimization
- [reactor_client.md](/applications/reactor/reactor_client.md): reactor pattern implementation

<!-- Reference links -->
[vcpkg]: https://vcpkg.io/
