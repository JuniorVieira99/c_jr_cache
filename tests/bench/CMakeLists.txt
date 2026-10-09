# Benchmarks comparing JrCache with other header-only C++ caches.
# Configure with the `bench` preset (Release + DE_BUILD_BENCHMARKS=ON).

# Google Benchmark: use an installed copy if there is one, otherwise download a pinned release.
find_package(benchmark CONFIG QUIET)
if(NOT benchmark_FOUND)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
        URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googlebenchmark)
endif()

add_executable(JrCacheBench bench_caches.cpp)
target_link_libraries(JrCacheBench PRIVATE JrCache::JrCache benchmark::benchmark)

# The libraries JrCache is compared with, vendored at pinned commits (see third_party/README.md).
target_include_directories(JrCacheBench SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/caches
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-lru-cache
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lrucache11)

if(MSVC)
    target_compile_options(JrCacheBench PRIVATE /W4)
else()
    target_compile_options(JrCacheBench PRIVATE -Wall -Wextra -Wpedantic)
endif()
