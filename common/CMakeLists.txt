#find_package(protobuf CONFIG REQUIRED)
#find_package(gRPC CONFIG REQUIRED)
#find_package(Threads)

include(FetchContent)
# For a better JSON parser and string minifier (the original code was to madly strip all spaces, no matter where)
FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v2.9.5  # Since v3.0.0, the lib moved to C++23 and requires CMake >= 3.25
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

#
# rg_common
#
add_library(rg_common
        "db_utils.cpp"
)
target_include_directories(rg_common
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)
target_link_libraries(rg_common
    PRIVATE
        glaze::glaze
        rg_proto_utils
        rg_proto
)
