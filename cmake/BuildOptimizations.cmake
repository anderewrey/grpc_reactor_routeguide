# BuildOptimizations.cmake
# Applies binary size optimizations for Release and MinSizeRel builds
#
# This module enables compiler and linker optimizations to reduce binary size:
#   - Function sections and data sections (for dead code elimination)
#   - Linker garbage collection (--gc-sections)
#   - Symbol stripping (-s)
#
# These optimizations are only applied to Release and MinSizeRel build types.

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    # Enable function/data sections for dead code elimination
    add_compile_options(-ffunction-sections -fdata-sections)

    # Enable linker garbage collection and strip symbols
    add_link_options(-Wl,--gc-sections -s)

    message(STATUS "Binary size optimizations enabled: function sections, gc-sections, symbol stripping")
endif()
