cmake_minimum_required(VERSION 3.25)

# Set up module path for custom CMake modules
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Auto-detect vcpkg installation (must be before project() to set toolchain)
include(VcpkgAutoDetect)

project(grpc-reactor-routeguide LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

# Enable CTest for GoogleTest integration
enable_testing()

# Verify compiler compatibility with system libraries (gRPC/Protobuf/Abseil)
include(CompilerCompatibilityCheck)

# Configure library search paths and RPATH
include(ConfigureRPath)

# Apply binary size optimizations for Release builds
include(BuildOptimizations)

# Find EventLoop for async applications
find_package(EventLoop CONFIG REQUIRED)

# Add subdirectories in dependency order
add_subdirectory(common)
add_subdirectory(protobuf_utils)
add_subdirectory(rg_service)
add_subdirectory(applications/blocking)
add_subdirectory(applications/callback)
add_subdirectory(applications/reactor)
