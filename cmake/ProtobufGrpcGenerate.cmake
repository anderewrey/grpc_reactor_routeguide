# ProtobufGrpcGenerate.cmake
# Helper function for generating protobuf and gRPC C++ code from .proto files
#
# Usage:
#   protobuf_grpc_generate(
#     TARGET <target_name>
#     PROTO_FILES <file1.proto> [<file2.proto> ...]
#     IMPORT_DIRS <dir1> [<dir2> ...]
#     OUTPUT_DIR <output_directory>
#   )
#
# This function generates both:
#   - Standard protobuf C++ files (.pb.h, .pb.cc)
#   - gRPC service C++ files (.grpc.pb.h, .grpc.pb.cc)

include(FindGrpcPlugin)

function(protobuf_grpc_generate)
    cmake_parse_arguments(
        ARG
        ""
        "TARGET;OUTPUT_DIR"
        "PROTO_FILES;IMPORT_DIRS"
        ${ARGN}
    )

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "protobuf_grpc_generate: TARGET argument required")
    endif()

    if(NOT ARG_PROTO_FILES)
        message(FATAL_ERROR "protobuf_grpc_generate: PROTO_FILES argument required")
    endif()

    if(NOT ARG_IMPORT_DIRS)
        message(FATAL_ERROR "protobuf_grpc_generate: IMPORT_DIRS argument required")
    endif()

    if(NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "protobuf_grpc_generate: OUTPUT_DIR argument required")
    endif()

    # Generate standard protobuf C++ files
    protobuf_generate(
        TARGET ${ARG_TARGET}
        LANGUAGE cpp
        IMPORT_DIRS ${ARG_IMPORT_DIRS}
        PROTOC_OUT_DIR ${ARG_OUTPUT_DIR}
    )

    # Generate gRPC service C++ files
    protobuf_generate(
        TARGET ${ARG_TARGET}
        LANGUAGE grpc
        GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
        PLUGIN "protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
        IMPORT_DIRS ${ARG_IMPORT_DIRS}
        PROTOC_OUT_DIR ${ARG_OUTPUT_DIR}
    )
endfunction()
