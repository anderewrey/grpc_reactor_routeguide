# Reactor pattern (Active Object) RouteGuide applications

# Client executable
add_executable(route_guide_active_reactor_client
    route_guide_active_reactor_client.cpp
    reactor_client.h
    reactor_client_routeguide.h
)

target_include_directories(route_guide_active_reactor_client
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_active_reactor_client
    PRIVATE
        rg_service
        EventLoop::EventLoop
)

# Copy database file to build directory
file(COPY ${CMAKE_SOURCE_DIR}/rg_service/route_guide_db.json
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)
