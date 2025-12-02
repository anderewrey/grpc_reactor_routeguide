# POC Tests for Reactor Client Testing Approaches
# Compares Option A (Real EventLoop) vs Option B (Synchronous/Promise-based)

# GoogleTest provided by vcpkg
find_package(GTest CONFIG REQUIRED)

# Option A: Real EventLoop + In-Process Server
add_executable(poc_option_a_eventloop
    poc_option_a_eventloop.cpp
)

target_include_directories(poc_option_a_eventloop
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(poc_option_a_eventloop
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
        EventLoop::EventLoop
)

# Option B: Synchronous Callbacks (No EventLoop)
add_executable(poc_option_b_sync
    poc_option_b_sync.cpp
)

target_include_directories(poc_option_b_sync
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(poc_option_b_sync
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rg_service
        # Note: No EventLoop dependency
)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(poc_option_a_eventloop)
gtest_discover_tests(poc_option_b_sync)
