include(FetchContent)
# For a better JSON parser and string minifier (the original code was to madly strip all spaces, no matter where)
FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        # with recent versions, moved to C++23 and requires CMake >= 3.25
        # and <expected> which is not generally available on compilers
        GIT_TAG v2.9.5
        GIT_SHALLOW TRUE
)
# Underlying logging system, instead of stdout and stderr.
FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glaze spdlog)

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
    PRIVATE
        glaze::glaze
        rg_proto_utils
        rg_proto
)
