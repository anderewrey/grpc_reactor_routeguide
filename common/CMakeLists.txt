include(FetchContent)
# For a better JSON parser and string minifier (the original code was to madly strip all spaces, no matter where)
FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        # with recent versions, it moved to C++23 and requires CMake >= 3.25, GCC12+/Clang17+,
        # and <expected> which is not generally available on compilers. For now, an older version of glaze
        # is good enough for our simple usages
        GIT_TAG v2.9.5
        GIT_SHALLOW TRUE
)
# Underlying logging system, instead of stdout and stderr.
FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
)
# Command-line parser
FetchContent_Declare(gflags
        GIT_REPOSITORY https://github.com/gflags/gflags.git
        GIT_TAG v2.2.2
        GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glaze spdlog gflags)

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
    PUBLIC
        spdlog::spdlog
        gflags::gflags
    PRIVATE
        glaze::glaze
        rg_proto_utils
        rg_proto
)
