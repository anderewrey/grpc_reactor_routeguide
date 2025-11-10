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

# Copy database file to build directory
file(COPY ${CMAKE_SOURCE_DIR}/rg_service/route_guide_db.json
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)
