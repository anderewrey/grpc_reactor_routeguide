# Find dependencies - Abseil first (required by protobuf and gRPC)
find_package(absl CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_IMPORT_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

#
# Add Library target with protobuf sources
#
add_library(rg_proto
    OBJECT
        ${PROTO_IMPORT_DIRS}/route_guide.proto)

# Link to gRPC libraries
# Note: OBJECT libraries don't automatically propagate INTERFACE properties from linked targets
# We need to manually copy INTERFACE_INCLUDE_DIRECTORIES to the OBJECT library
target_link_libraries(rg_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
)

# OBJECT libraries need explicit include directories
get_target_property(GRPC_INCLUDE_DIRS gRPC::grpc++ INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(PROTO_INCLUDE_DIRS protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(rg_proto PUBLIC ${GRPC_INCLUDE_DIRS} ${PROTO_INCLUDE_DIRS})

target_include_directories(rg_proto
    PUBLIC
        ${PROTO_GENERATED_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)

#
# Compile protobuf and grpc files in rg_proto target to cpp
#
# Find grpc_cpp_plugin
if(TARGET gRPC::grpc_cpp_plugin)
    # System installation exports the target
    get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
    message(STATUS "Found grpc_cpp_plugin from target: ${grpc_cpp_plugin_location}")
else()
    # Search in standard locations
    find_program(grpc_cpp_plugin_location
        NAMES grpc_cpp_plugin
        REQUIRED
    )
    message(STATUS "Found grpc_cpp_plugin from system: ${grpc_cpp_plugin_location}")
endif()
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
    PUBLIC
        rg_proto  # PUBLIC exports rg_proto's interface including all its PUBLIC dependencies
)

# OBJECT libraries don't always propagate INTERFACE_LINK_LIBRARIES properly
# Explicitly add gRPC libraries to ensure they propagate to final executables
target_link_libraries(rg_proto_utils PUBLIC protobuf::libprotobuf gRPC::grpc++ gRPC::grpc)
