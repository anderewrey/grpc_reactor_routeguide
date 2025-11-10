# Generic protobuf utilities library

find_package(protobuf CONFIG REQUIRED)

add_library(protobuf_utils
    protobuf_utils.cpp
)

target_include_directories(protobuf_utils
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(protobuf_utils
    PUBLIC
        protobuf::libprotobuf
)
