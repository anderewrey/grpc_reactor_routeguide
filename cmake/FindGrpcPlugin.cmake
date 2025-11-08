# FindGrpcPlugin.cmake
# Finds the grpc_cpp_plugin executable required for gRPC code generation
#
# This module locates grpc_cpp_plugin from either:
#   1. gRPC::grpc_cpp_plugin target (if available)
#   2. System PATH
#
# Output variables:
#   GRPC_CPP_PLUGIN - Path to grpc_cpp_plugin executable

if(NOT GRPC_CPP_PLUGIN)
    if(TARGET gRPC::grpc_cpp_plugin)
        # System installation exports the target
        get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
        message(STATUS "Found grpc_cpp_plugin from target: ${GRPC_CPP_PLUGIN}")
    else()
        # Search in standard locations
        find_program(GRPC_CPP_PLUGIN
            NAMES grpc_cpp_plugin
            REQUIRED
        )
        message(STATUS "Found grpc_cpp_plugin from system: ${GRPC_CPP_PLUGIN}")
    endif()
endif()
