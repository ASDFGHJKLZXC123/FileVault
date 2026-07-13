cmake_minimum_required(VERSION 3.24)

project(
    LocalVault
    VERSION 0.1.0
    DESCRIPTION "Snapshot backup and deduplication application"
    LANGUAGES C CXX
)

include(CTest)

option(LOCALVAULT_BUILD_GUI "Build the Qt desktop application" ON)
option(LOCALVAULT_BUILD_BENCHMARKS "Build benchmark tools" ON)
option(LOCALVAULT_WARNINGS_AS_ERRORS "Treat LocalVault warnings as errors" OFF)
option(LOCALVAULT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LOCALVAULT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LOCALVAULT_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
    add_link_options(-fexperimental-library)
endif()

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/localvault")
configure_file(
    "${PROJECT_SOURCE_DIR}/include/localvault/version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/localvault/version.hpp"
    @ONLY
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Dependencies)
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(LOCALVAULT_BUILD_GUI)
    add_subdirectory(src/desktop)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(LOCALVAULT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
