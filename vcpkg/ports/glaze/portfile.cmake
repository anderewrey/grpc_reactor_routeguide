# Custom glaze port pinned to v2.9.5 for C++20 compatibility
# v3.x requires C++23 and very recent compilers

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO stephenberry/glaze
    REF "v2.9.5"
    SHA512 c84f158676e6956ccb587b1683b54ba549813340efbb81708859e3bcd7b6a9c337223f91ba41a0ebd95eb0c3c3f5262a0819df3ead86830c06f0d68cf855adcb
    HEAD_REF main
)

# Configure and install using CMake (glaze provides CMakeLists.txt)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dglaze_DEVELOPER_MODE=OFF
)

vcpkg_cmake_install()

# Fix CMake config file locations
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/glaze)

# Remove empty lib directory (header-only library)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug" "${CURRENT_PACKAGES_DIR}/lib")

# Install license
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
