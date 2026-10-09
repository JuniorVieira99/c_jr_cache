# Catch2 v3 comes from vcpkg through the toolchain file set in CMakePresets.json.
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(JrCacheTests
    test_linked_list.cpp
    test_bucket_map.cpp
    test_cache_map.cpp
    test_utilities.cpp
    test_sharded_map.cpp
    test_serialization.cpp)

target_link_libraries(JrCacheTests PRIVATE JrCache::JrCache Catch2::Catch2WithMain)

if(MSVC)
    # Covers both cl and clang-cl (where -Wall would mean -Weverything).
    target_compile_options(JrCacheTests PRIVATE /W4)
else()
    target_compile_options(JrCacheTests PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(JrCacheTests PROPERTIES COMPILE_WARNING_AS_ERROR ${DE_WARNINGS_AS_ERRORS})

# Registers every TEST_CASE with CTest so `ctest --preset debug` runs them individually.
catch_discover_tests(JrCacheTests)
