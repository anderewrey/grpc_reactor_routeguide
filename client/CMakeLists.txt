include(FetchContent)
# Minimalist EventLoop support
FetchContent_Declare(EventLoop
        GIT_REPOSITORY https://github.com/amoldhamale1105/EventLoop.git
        GIT_TAG e16fa52aae1d58a994fd93808c62fde14adb62f1 # Includes my patch, no tag yet available
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED 1
)
FetchContent_MakeAvailable(EventLoop)

# Targets route_guide_[sync|callback|proxy_callback]_client
foreach(_target sync callback proxy_callback)
    add_executable(route_guide_${_target}_client
            "route_guide_${_target}_client.cpp")
    target_include_directories(route_guide_${_target}_client
        PRIVATE
            ${EventLoop_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/common
            ${CMAKE_SOURCE_DIR}/proto
            ${CMAKE_SOURCE_DIR}
    )
    target_link_libraries(route_guide_${_target}_client
        PRIVATE
            rg_proto_utils
            rg_proto
            rg_common
            EventLoop)
endforeach()

file(COPY ${CMAKE_SOURCE_DIR}/common/route_guide_db.json
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)
