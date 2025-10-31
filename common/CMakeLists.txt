# Use vcpkg-provided packages for third-party dependencies
# vcpkg builds them in optimized Release mode even when the application is built in Debug mode,
# as configured in custom triplets (x64-linux-clang19-release, x64-linux-gcc13-release).

# JSON parser - using v2.9.5 for C++20 compatibility (v3+ requires C++23)
find_package(glaze CONFIG REQUIRED)

# Logging library
find_package(spdlog CONFIG REQUIRED)

# Command-line argument parser
find_package(gflags CONFIG REQUIRED)

#
# rg_common
#
add_library(rg_common
        "db_utils.cpp"
)
target_include_directories(rg_common
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)
target_link_libraries(rg_common
    PUBLIC
        spdlog::spdlog
        gflags::gflags
    PRIVATE
        glaze::glaze
        rg_proto_utils  # Transitively provides rg_proto and its dependencies
)
