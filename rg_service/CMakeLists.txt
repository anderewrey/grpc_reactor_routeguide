# Find dependencies - Abseil first (required by protobuf and gRPC)
find_package(absl CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
# vcpkg's gflags defaults to non-namespaced targets. This enables gflags::gflags target.
set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
find_package(gflags CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# Include protobuf/gRPC generation helper
include(ProtobufGrpcGenerate)

set(PROTO_IMPORT_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

#
# Library 1: rg_proto (generated protobuf/gRPC code)
#
add_library(rg_proto
    OBJECT
        ${PROTO_IMPORT_DIRS}/route_guide.proto
)

# Link to gRPC libraries
# Note: OBJECT libraries don't automatically propagate INTERFACE properties from linked targets
target_link_libraries(rg_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
)

target_include_directories(rg_proto
    PUBLIC
        ${PROTO_GENERATED_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        $<TARGET_PROPERTY:gRPC::grpc++,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:protobuf::libprotobuf,INTERFACE_INCLUDE_DIRECTORIES>
)

#
# Generate protobuf and gRPC C++ code from .proto files
#
protobuf_grpc_generate(
    TARGET rg_proto
    PROTO_FILES ${PROTO_IMPORT_DIRS}/route_guide.proto
    IMPORT_DIRS ${PROTO_IMPORT_DIRS}
    OUTPUT_DIR ${PROTO_GENERATED_DIR}
)

#
# Library 2: rg_service (RouteGuide business logic)
#
add_library(rg_service
    rg_utils.cpp
    rg_db.cpp
    rg_logger.cpp
    route_guide_service.h
    rg_logger.h
)

target_include_directories(rg_service
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(rg_service
    PUBLIC
        rg_proto
        protobuf_utils
        gflags::gflags
        common
    PRIVATE
        spdlog::spdlog
        glaze::glaze
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc
)

# OBJECT libraries don't always propagate INTERFACE_LINK_LIBRARIES properly
# Explicitly add gRPC libraries to ensure they propagate to final executables
target_link_libraries(rg_service PUBLIC protobuf::libprotobuf gRPC::grpc++ gRPC::grpc)
