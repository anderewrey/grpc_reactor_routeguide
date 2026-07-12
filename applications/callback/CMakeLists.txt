# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-2026 anderewrey

# Asynchronous callback-based RouteGuide applications

# Client executable
add_executable(route_guide_callback_client
    route_guide_callback_client.cpp
)

target_include_directories(route_guide_callback_client
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_callback_client
    PRIVATE
        rg_service
)

# Server executable
add_executable(route_guide_callback_server
    route_guide_callback_server.cpp
)

target_include_directories(route_guide_callback_server
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_callback_server
    PRIVATE
        rg_service
)
