# Custom gRPC port for grpc_reactor_routeguide project
# Based on GRPC_BUILD_GUIDE.md specifications
# Version: 1.73.1
# C++ only, speed-optimized, minimal features

# Why we call git directly instead of using vcpkg_from_git or vcpkg_from_github:
#
# 1. vcpkg_from_github downloads GitHub release tarballs, which don't include submodules
# 2. vcpkg_from_git uses "git archive" internally, which explicitly excludes submodules
# 3. gRPC requires submodules for module providers (Protobuf, Abseil, c-ares, RE2)
# 4. Direct git clone is the official vcpkg solution for projects requiring submodules
#    See: https://github.com/microsoft/vcpkg/issues/6886
#         https://github.com/microsoft/vcpkg/issues/1036
#
# This approach matches GRPC_BUILD_GUIDE.md: git clone --depth 1 --recurse-submodules

set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/grpc-v1.73.1")

if(NOT EXISTS "${SOURCE_PATH}/.git")
    find_program(GIT git)

    message(STATUS "Cloning gRPC v1.73.1 with submodules...")
    file(MAKE_DIRECTORY "${CURRENT_BUILDTREES_DIR}/src")

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

# Build gRPC with submodules (module providers)
# This ensures version compatibility of the gRPC bundle:
# gRPC + Protobuf + Abseil + c-ares + RE2

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # Installation
        -DgRPC_INSTALL=ON
        -DgRPC_BUILD_TESTS=OFF

        # C++ only - disable all other language plugins
        -DgRPC_BUILD_CODEGEN=ON
        -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON
        -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF
        -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF
        -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF
        -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF
        -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF
        -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF
        -DgRPC_BUILD_GRPCPP_OTEL_PLUGIN=OFF

        # Disable OpenTelemetry plugin completely
        -DgRPC_USE_PROTO_LITE=OFF

        # Dependency providers
        # Use vcpkg packages (consistent management, security updates)
        -DgRPC_SSL_PROVIDER=package
        -DgRPC_ZLIB_PROVIDER=package

        # Build from submodules (version-matched gRPC bundle)
        -DgRPC_PROTOBUF_PROVIDER=module
        -DgRPC_ABSL_PROVIDER=module
        -DgRPC_CARES_PROVIDER=module
        -DgRPC_RE2_PROVIDER=module

        # Protobuf options
        -Dprotobuf_BUILD_TESTS=OFF
        -Dprotobuf_WITH_ZLIB=ON
        -Dprotobuf_ALLOW_CCACHE=ON

        # Disable unnecessary features
        -DgRPC_BENCHMARK_PROVIDER=none
        -DgRPC_DOWNLOAD_ARCHIVES=OFF

        # Install paths
        -DgRPC_INSTALL_BINDIR:STRING=tools/grpc
        -DgRPC_INSTALL_LIBDIR:STRING=lib
        -DgRPC_INSTALL_INCLUDEDIR:STRING=include
        -DgRPC_INSTALL_CMAKEDIR:STRING=share/grpc
)

vcpkg_cmake_install(ADD_BIN_TO_PATH)
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup()

# Copy grpc_cpp_plugin to tools
vcpkg_copy_tools(
    TOOL_NAMES
        grpc_cpp_plugin
    SEARCH_DIR "${CURRENT_PACKAGES_DIR}/tools/grpc"
    AUTO_CLEAN
)

# Remove grpcpp_otel_plugin.pc (references missing opentelemetry_api)
file(REMOVE "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/grpcpp_otel_plugin.pc")

# Fix pkgconfig
vcpkg_fixup_pkgconfig()

# Remove debug include files
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Install license
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
