cmake_minimum_required(VERSION 3.25)

project(JrCache
    VERSION 0.1.0
    DESCRIPTION "A Multi-Purpose Cache for C++ applications"
    LANGUAGES CXX)

# ---- Guard against in-source builds -----------------------------------------
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Configure with a preset instead, e.g.:\n"
        "    cmake --preset debug")
endif()

# ---- Global language settings -----------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd understands the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ---- Project options --------------------------------------------------------
option(DE_BUILD_TESTS "Build the Catch2 unit tests" ON)
option(DE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DE_BUILD_BENCHMARKS "Build the benchmarks in tests/bench (downloads Google Benchmark)" OFF)

# ---- Library ----------------------------------------------------------------
# Header-only, so an INTERFACE target: anything that links it gets the include path.
add_library(JrCache INTERFACE)
add_library(JrCache::JrCache ALIAS JrCache)
target_include_directories(JrCache INTERFACE ${PROJECT_SOURCE_DIR}/headers)
# Bundled nlohmann/json 3.12.0 (single header) for serialization.
target_include_directories(JrCache SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party)
target_compile_features(JrCache INTERFACE cxx_std_20)

# ---- Tests ------------------------------------------------------------------
if(DE_BUILD_TESTS AND EXISTS ${PROJECT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Benchmarks -------------------------------------------------------------
if(DE_BUILD_BENCHMARKS)
    add_subdirectory(tests/bench)
endif()

