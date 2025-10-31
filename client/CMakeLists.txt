# Minimalist EventLoop support - using vcpkg overlay port
# Pinned to commit e16fa52aae1d58a994fd93808c62fde14adb62f1 (includes required patches)
find_package(EventLoop CONFIG REQUIRED)

# Targets route_guide_[sync|callback|proxy_callback]_client
foreach(_target sync callback proxy_callback)
    add_executable(route_guide_${_target}_client
            "route_guide_${_target}_client.cpp")
    target_include_directories(route_guide_${_target}_client
        PRIVATE
            ${CMAKE_SOURCE_DIR}/common
            ${CMAKE_SOURCE_DIR}/proto
            ${CMAKE_SOURCE_DIR}
    )
    target_link_libraries(route_guide_${_target}_client
        PRIVATE
            rg_proto_utils
            rg_common
            EventLoop::EventLoop)
endforeach()

file(COPY ${CMAKE_SOURCE_DIR}/common/route_guide_db.json
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)
