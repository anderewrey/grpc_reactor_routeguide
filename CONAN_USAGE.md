# Conan Package Manager Integration

**⚠️ STATUS: ABANDONED**

This approach was investigated in October 2025 but ultimately not adopted.

## Why It Was Abandoned

Conan 2.x builds packages in Release mode and generates CMake config files with config-specific generator expressions (`$<$<CONFIG:Release>:...>`). This causes issues when building the application in Debug mode:

- Include directories from Conan are not added (filtered out by generator expressions)
- Libraries from Conan are not linked (filtered out by generator expressions)
- Build falls back to system libraries, causing ABI conflicts

While Conan works perfectly for **Release builds**, the inability to use Release-built packages in Debug application builds made it unsuitable for development workflows.

## Alternative Chosen

**Manual Compiler-Specific Installations** (Option 4 in TODOS.md)

Build gRPC/Protobuf/Abseil manually for each compiler into separate prefixes:
- Clang builds: `/usr/local/clang/`
- GCC builds: `/usr/local/`

This provides:
- Full control over Debug/Release configurations
- No package manager complexity
- Works for any build type
- Simple, predictable library paths

## Investigation History

See **TODOS.md Task 5** for complete investigation details, attempted fixes, and rationale for the decision.

## Files Removed

All Conan-related files have been removed from the project:
- `conan/` directory (recipes, profiles)
- `conanfile.py` (project root)
- `conan_provider.cmake`
- Conan-specific CMake conditionals

The CMake build system has been simplified to use system-installed libraries only.
