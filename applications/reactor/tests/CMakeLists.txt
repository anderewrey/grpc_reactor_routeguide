# SPDX-License-Identifier: Apache-2.0
# Copyright 2024 anderewrey

# Reactor Client Tests

find_package(GTest REQUIRED)
find_package(EventLoop CONFIG REQUIRED)

set(REACTOR_TESTS
    active_unary_reactor_test
    active_read_reactor_test
    active_write_reactor_test
    active_bidi_reactor_test
    client_reactor_integration_test
)

foreach(test_name IN LISTS REACTOR_TESTS)
    add_executable(${test_name}
        ${test_name}.cpp
    )

    target_include_directories(${test_name}
        PRIVATE
            ${CMAKE_SOURCE_DIR}
    )

    # Strip the absolute source path from __FILE__ so GoogleTest's JUnit XML
    # (and any report built from it) shows a short relative path instead of
    # wherever the repo happened to be checked out.
    target_compile_options(${test_name}
        PRIVATE
            -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=
    )

    target_link_libraries(${test_name}
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            rg_service
    )
endforeach()

# Only the EventLoop integration test needs the real EventLoop library
target_link_libraries(client_reactor_integration_test
    PRIVATE
        EventLoop::EventLoop
)

include(GoogleTest)
foreach(test_name IN LISTS REACTOR_TESTS)
    gtest_discover_tests(${test_name}
        XML_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results
    )
endforeach()
