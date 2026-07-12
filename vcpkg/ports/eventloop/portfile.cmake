# Pinned to a commit on the anderewrey/EventLoop fork's cmake-improvement branch, not upstream
# amoldhamale1105/EventLoop, since that branch carries the LANGUAGES CXX restriction, install()
# rules, and GNUInstallDirs fixes this project needs, proposed upstream but not yet merged
# (see TODOS.md). No patches needed here - the fork commit already has the fixes applied directly.
# Revert REPO/REF/SHA512 to upstream and drop this comment once the PR is accepted.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO anderewrey/EventLoop
    REF d5d29a0349f3159067df0c3ff2741f1e984679f0
    SHA512 311f2d167d26c0afbb92b905622ceefb801be06ea6fe9f41569cc476d8eb6c10fccc9b13daae0bc86d6d2b2ffe877e251c0577d4fe5fe3b1c541641edae08c0c
    HEAD_REF master
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
