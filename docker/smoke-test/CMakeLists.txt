cmake_minimum_required(VERSION 3.25)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

project(smoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(absl CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

include(ProtobufGrpcGenerate)

set(PROTO_IMPORT_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_library(smoke_proto OBJECT ${PROTO_IMPORT_DIRS}/smoke.proto)

target_link_libraries(smoke_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
)

target_include_directories(smoke_proto
    PUBLIC
        ${PROTO_GENERATED_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        $<TARGET_PROPERTY:gRPC::grpc++,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:protobuf::libprotobuf,INTERFACE_INCLUDE_DIRECTORIES>
)

protobuf_grpc_generate(
    TARGET smoke_proto
    PROTO_FILES ${PROTO_IMPORT_DIRS}/smoke.proto
    IMPORT_DIRS ${PROTO_IMPORT_DIRS}
    OUTPUT_DIR ${PROTO_GENERATED_DIR}
)

add_executable(smoke_test main.cpp)
target_link_libraries(smoke_test PRIVATE smoke_proto)
