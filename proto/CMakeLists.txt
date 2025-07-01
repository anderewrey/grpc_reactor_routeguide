find_package(gRPC)

set(PROTO_IMPORT_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

#
# Add Library target with protobuf sources
#
add_library(rg_proto
    OBJECT
        ${PROTO_IMPORT_DIRS}/route_guide.proto)

target_link_libraries(rg_proto
    PUBLIC
        grpc::grpc
)

target_include_directories(rg_proto
    PUBLIC
        ${PROTO_GENERATED_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

#
# Compile protobuf and grpc files in rg_proto target to cpp
#
get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(
        TARGET rg_proto
        LANGUAGE cpp
        IMPORT_DIRS ${PROTO_IMPORT_DIRS}
        PROTOC_OUT_DIR ${PROTO_GENERATED_DIR}
)
protobuf_generate(
        TARGET rg_proto
        LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}"
        IMPORT_DIRS ${PROTO_IMPORT_DIRS}
        PROTOC_OUT_DIR ${PROTO_GENERATED_DIR}
)

#
# proto_utils
#
add_library(rg_proto_utils
        "proto_utils.cpp"
)
target_include_directories(rg_proto_utils
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)
target_link_libraries(rg_proto_utils
    PRIVATE
        rg_proto
)
