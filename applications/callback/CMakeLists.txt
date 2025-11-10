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
        EventLoop::EventLoop
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

# Copy database file to build directory
file(COPY ${CMAKE_SOURCE_DIR}/rg_service/route_guide_db.json
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)
