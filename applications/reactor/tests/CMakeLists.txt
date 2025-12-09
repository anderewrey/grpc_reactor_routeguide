# Reactor Client Tests

find_package(GTest CONFIG REQUIRED)

add_executable(active_unary_reactor_test
    active_unary_reactor_test.cpp
)

target_include_directories(active_unary_reactor_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(active_unary_reactor_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
)

add_executable(active_read_reactor_test
    active_read_reactor_test.cpp
)

target_include_directories(active_read_reactor_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(active_read_reactor_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
)

add_executable(active_write_reactor_test
    active_write_reactor_test.cpp
)

target_include_directories(active_write_reactor_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(active_write_reactor_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
)

find_package(EventLoop CONFIG REQUIRED)

add_executable(client_reactor_integration_test
    client_reactor_integration_test.cpp
)

target_include_directories(client_reactor_integration_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(client_reactor_integration_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
        EventLoop::EventLoop
)

add_executable(active_bidi_reactor_test
    active_bidi_reactor_test.cpp
)

target_include_directories(active_bidi_reactor_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(active_bidi_reactor_test
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
)

include(GoogleTest)
gtest_discover_tests(active_unary_reactor_test)
gtest_discover_tests(active_read_reactor_test)
gtest_discover_tests(active_write_reactor_test)
gtest_discover_tests(active_bidi_reactor_test)
gtest_discover_tests(client_reactor_integration_test)
