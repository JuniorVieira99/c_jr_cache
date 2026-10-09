#include "cache_sharded_map.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using JrCache::EvictionPolicy;
using Sharded = JrCache::ShardedMap<int, std::string>;
using Item = std::pair<int, std::string>;

TEST_CASE("ShardedMap rejects zero shards or zero capacity", "[ShardedMap]") {
    CHECK_THROWS_AS(Sharded(0, 10, EvictionPolicy::LRU), std::invalid_argument);
    CHECK_THROWS_AS(Sharded(4, 0, EvictionPolicy::LRU), std::invalid_argument);
}

TEST_CASE("ShardedMap reports its settings", "[ShardedMap]") {
    Sharded map(4, 10, EvictionPolicy::LFU);

    CHECK(map.Capacity() == 40);
    CHECK(map.GetEvictionPolicy() == EvictionPolicy::LFU);
}

TEST_CASE("ShardedMap basic operations work under every policy", "[ShardedMap]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::MRU, EvictionPolicy::FIFO,
                           EvictionPolicy::LIFO, EvictionPolicy::LFU, EvictionPolicy::MFU);
    CAPTURE(policy);
    // Large enough that no shard fills up, whichever way the keys are spread.
    Sharded map(4, 100, policy);

    SECTION("a new map is empty") {
        CHECK(map.IsEmpty());
        CHECK(map.Size() == 0);
        CHECK_FALSE(map.Contains(1));
        CHECK_FALSE(map.Get(1).has_value());
        CHECK_FALSE(map.Remove(1).has_value());
    }
    SECTION("Put, Get, Contains and Remove") {
        for (int key = 0; key < 50; ++key) {
            CHECK_FALSE(map.Put(key, std::to_string(key)).has_value());
        }
        CHECK(map.Size() == 50);
        CHECK_FALSE(map.IsEmpty());
        for (int key = 0; key < 50; ++key) {
            CHECK(map.Contains(key));
            CHECK(map.Get(key) == std::to_string(key));
        }

        CHECK(map.Remove(7) == Item{7, "7"});
        CHECK_FALSE(map.Contains(7));
        CHECK(map.Size() == 49);
    }
    SECTION("Put on an existing key replaces the value") {
        map.Put(1, "a");
        map.Put(1, "b");
        CHECK(map.Size() == 1);
        CHECK(map.Get(1) == "b");
    }
    SECTION("Clear empties every shard") {
        for (int key = 0; key < 50; ++key) {
            map.Put(key, "v");
        }
        map.Clear();
        CHECK(map.IsEmpty());
        CHECK(map.Size() == 0);
    }
}

TEST_CASE("ShardedMap with one shard behaves like a CacheMap", "[ShardedMap]") {
    Sharded map(1, 2, EvictionPolicy::LRU);
    map.Put(1, "a");
    map.Put(2, "b");
    (void)map.Get(1);

    CHECK(map.Put(3, "c") == Item{2, "b"});
    CHECK(map.Size() == 2);
}

TEST_CASE("ShardedMap never holds more than its total capacity", "[ShardedMap]") {
    Sharded map(4, 5, EvictionPolicy::FIFO);

    for (int key = 0; key < 1000; ++key) {
        auto evicted = map.Put(key, std::to_string(key));
        if (evicted) {
            CHECK_FALSE(map.Contains(evicted->first));
        }
        REQUIRE(map.Size() <= map.Capacity());
    }
    // With 1000 keys every shard has filled up.
    CHECK(map.Size() == map.Capacity());
}

TEST_CASE("ShardedMap spreads keys that share a common factor", "[ShardedMap]") {
    // Keys 0, 8, 16, ... would all land in one shard if the raw std::hash<int> were used
    // with 8 shards on GCC or Clang, leaving room for only 10 of them.
    Sharded map(8, 10, EvictionPolicy::LRU);
    for (int i = 0; i < 40; ++i) {
        map.Put(i * 8, "v");
    }

    CHECK(map.Size() > 10);
}

TEST_CASE("ShardedMap stays consistent across threads", "[ShardedMap][threads]") {
    constexpr int thread_count = 4;
    constexpr int operations_per_thread = 2000;
    constexpr int key_range = 300;

    Sharded map(8, 16, EvictionPolicy::LRU);
    // Catch2 assertions are not thread-safe, so the workers only count wrong values.
    std::atomic<int> wrong_values{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&map, &wrong_values, t] {
            for (int i = 0; i < operations_per_thread; ++i) {
                const int key = (t * operations_per_thread + i) % key_range;
                map.Put(key, std::to_string(key));
                auto value = map.Get(key / 2);
                if (value && *value != std::to_string(key / 2)) {
                    ++wrong_values;
                }
                if (i % 5 == 0) {
                    map.Remove(key / 3);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(wrong_values == 0);
    CHECK(map.Size() <= map.Capacity());
    std::size_t found = 0;
    for (int key = 0; key < key_range; ++key) {
        if (map.Contains(key)) {
            ++found;
            CHECK(map.Get(key) == std::to_string(key));
        }
    }
    CHECK(found == map.Size());
}

TEST_CASE("ShardedMap GetPtr and Visit reach the cached value", "[ShardedMap][GetPtr]") {
    Sharded map(4, 10, EvictionPolicy::LRU);
    map.Put(1, "a");

    CHECK(map.GetPtr(9) == nullptr);
    std::string* value = map.GetPtr(1);
    REQUIRE(value != nullptr);
    value->append("!");
    CHECK(map.Get(1) == "a!");

    std::string seen;
    CHECK(map.Visit(1, [&](std::string& v) { seen = v; }));
    CHECK(seen == "a!");
    CHECK_FALSE(map.Visit(9, [](std::string&) {}));
}
