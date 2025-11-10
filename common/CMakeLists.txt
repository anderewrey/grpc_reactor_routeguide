# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-2025 anderewrey

# Header-only library for common C++ compatibility utilities
add_library(common INTERFACE)

target_include_directories(common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
