# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-2026 anderewrey

# Synchronous (blocking) RouteGuide applications

# Client executable
add_executable(route_guide_sync_client
    route_guide_sync_client.cpp
)

target_include_directories(route_guide_sync_client
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_sync_client
    PRIVATE
        rg_service
)

# Server executable
add_executable(route_guide_sync_server
    route_guide_sync_server.cpp
)

target_include_directories(route_guide_sync_server
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_sync_server
    PRIVATE
        rg_service
)
