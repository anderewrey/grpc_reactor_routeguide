# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 anderewrey

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

# Smoke test: the real client against the real server, so CI's sanitizer jobs exercise both.
# Both bind or dial the fixed port 50051, hence the lock against any other test that would use it.
add_test(NAME callback_smoke_test
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/callback_smoke_test.sh
        $<TARGET_FILE:route_guide_callback_server> $<TARGET_FILE:route_guide_callback_client>
)
set_tests_properties(callback_smoke_test PROPERTIES
    TIMEOUT 300
    RESOURCE_LOCK routeguide_port_50051
)
