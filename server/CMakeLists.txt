find_package(Threads)

# Targets route_guide_[sync|callback|proxy_callback]_server
foreach(_target sync callback)
    add_executable(route_guide_${_target}_server
            "route_guide_${_target}_server.cpp")
    target_include_directories(route_guide_${_target}_server
        PRIVATE
            ${CMAKE_SOURCE_DIR}/common
            ${CMAKE_SOURCE_DIR}/proto
            ${CMAKE_SOURCE_DIR}
    )
    target_link_libraries(route_guide_${_target}_server
        PRIVATE
            rg_proto_utils
            rg_common)
endforeach()

file(COPY ${CMAKE_SOURCE_DIR}/common/route_guide_db.json
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)
