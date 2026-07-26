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

# Reproduction test for a suspected use-after-free in route_guide_callback_server.cpp (see
# tests/run_uaf_repro.sh): runs the real client against the real server and checks the server's
# own output for a sanitizer report. Only the clang-asan-ubsan CI variant instruments memory
# accesses, so this is a smoke test there and a plain interop smoke test on the other variants.
add_test(
    NAME callback_server_use_after_free_repro
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_uaf_repro.sh
            $<TARGET_FILE:route_guide_callback_server>
            $<TARGET_FILE:route_guide_callback_client>
)
set_tests_properties(callback_server_use_after_free_repro PROPERTIES TIMEOUT 90)
