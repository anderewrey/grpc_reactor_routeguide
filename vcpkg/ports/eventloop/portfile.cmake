# VERSION is EventLoop's release tag, exactly as upstream names it, set in vcpkg.json as a version-string:
# vcpkg's numeric version schemes would reject its leading "v". This port is the one place that names the
# EventLoop the project builds against: every workflow installs it through vcpkg.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO amoldhamale1105/EventLoop
    REF "${VERSION}"
    SHA512 c74ffb45678a9ea52b81688d48aa9d155ffab19ce3d00a29f2d624bd5165c2e7c20d7eb230b3ced271fd62fbd92fcb702cda6753315ae5298b3364782a97e33d
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/EventLoop)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
