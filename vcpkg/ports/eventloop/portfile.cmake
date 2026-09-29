# EventLoop has no tagged releases, so REF is pinned to a commit rather than derived from VERSION.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO amoldhamale1105/EventLoop
    REF e967d5e6f6856784c77e3aaa35fd39374a275723
    SHA512 6d2b23eca3f2aeaea2425c5a4f631f8958bf2c39db18108e9308c4e8151308beb5d4d1fa9bda83c64b220ad9420cbcfc0e94c177fdcd5bd36f863fe66f9baf15
    HEAD_REF master
    # Upstream pushes onto the event queue without the mutex eventLoop() reads it under, a data race
    # TSan reports. To be proposed upstream.
    PATCHES
        fix-thread-safety.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/EventLoop)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
