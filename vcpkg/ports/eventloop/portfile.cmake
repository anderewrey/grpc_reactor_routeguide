# EventLoop has no tagged releases, so REF is pinned to a commit rather than derived from VERSION.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO amoldhamale1105/EventLoop
    REF ce1c71eb09ae97dd1672fff90f03b94c02c22649
    SHA512 66c819b0f2e7d8eac9421dfa01ca23e824f5d00e76b37b89253538a4aef0f94badeec0202151bb5e7a05d619387702d84b2b118c87331354ae81a6e0faf7d49a
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/EventLoop)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
