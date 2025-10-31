# Custom EventLoop port pinned to specific commit with required patches
# No release tag available for this commit (e16fa52aae1d58a994fd93808c62fde14adb62f1)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO amoldhamale1105/EventLoop
    REF e16fa52aae1d58a994fd93808c62fde14adb62f1
    SHA512 cc233c1d29162716987a6f84f491d9a9550cf789879ea85d5a33309e66b6cf65b999ce19cb68d736bfc9e2b29f96e9d78cd08227d0aba58fb9aaf0d93432c202
    HEAD_REF main
    PATCHES
        add-install-rules.patch
)

# Configure and install using CMake
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

# Fix CMake config file locations
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/EventLoop)

# Remove empty directories if header-only
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# Install license
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")