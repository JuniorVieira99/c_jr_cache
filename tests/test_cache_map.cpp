#include "cache_map.hpp"
#include "utilities.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using JrCache::CacheMap;
using JrCache::EvictionPolicy;
using Cache = CacheMap<int, std::string>;
using Item = std::pair<int, std::string>;

namespace {

// Fills a cache of capacity 3 with keys 1, 2 and 3, in that order.
void putThree(Cache& cache) {
    REQUIRE_FALSE(cache.Put(1, "a").has_value());
    REQUIRE_FALSE(cache.Put(2, "b").has_value());
    REQUIRE_FALSE(cache.Put(3, "c").has_value());
}

} // namespace

// ---- Behaviour shared by every policy -----------------------------------------

TEST_CASE("CacheMap rejects a capacity of zero", "[CacheMap]") {
    CHECK_THROWS_AS(Cache(0), std::invalid_argument);
}

TEST_CASE("CacheMap reports its settings", "[CacheMap]") {
    Cache cache(5, EvictionPolicy::MFU);

    CHECK(cache.Capacity() == 5);
    CHECK(cache.GetEvictionPolicy() == EvictionPolicy::MFU);
    CHECK(Cache(1).GetEvictionPolicy() == EvictionPolicy::LRU);
}

TEST_CASE("CacheMap basic operations work under every policy", "[CacheMap]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::MRU, EvictionPolicy::FIFO,
                           EvictionPolicy::LIFO, EvictionPolicy::LFU, EvictionPolicy::MFU);
    CAPTURE(policy);
    Cache cache(3, policy);

    SECTION("a new cache is empty") {
        CHECK(cache.IsEmpty());
        CHECK(cache.Size() == 0);
        CHECK_FALSE(cache.Contains(1));
        CHECK_FALSE(cache.Get(1).has_value());
        CHECK_FALSE(cache.Remove(1).has_value());
    }
    SECTION("Put then Get returns the value") {
        putThree(cache);
        CHECK(cache.Size() == 3);
        CHECK(cache.Contains(2));
        CHECK(cache.Get(1) == "a");
        CHECK(cache.Get(2) == "b");
        CHECK(cache.Get(3) == "c");
    }
    SECTION("Put on an existing key replaces the value without evicting") {
        putThree(cache);
        CHECK_FALSE(cache.Put(2, "b2").has_value());
        CHECK(cache.Size() == 3);
        CHECK(cache.Get(2) == "b2");
    }
    SECTION("Remove returns the pair and frees a slot") {
        putThree(cache);
        CHECK(cache.Remove(2) == Item{2, "b"});
        CHECK_FALSE(cache.Contains(2));
        CHECK_FALSE(cache.Remove(2).has_value());
        CHECK(cache.Size() == 2);

        CHECK_FALSE(cache.Put(4, "d").has_value());
        CHECK(cache.Size() == 3);
        CHECK(cache.Get(1) == "a");
        CHECK(cache.Get(4) == "d");
    }
    SECTION("a full cache evicts exactly one entry per new key") {
        putThree(cache);
        for (int key = 4; key < 20; ++key) {
            auto evicted = cache.Put(key, std::to_string(key));
            REQUIRE(evicted.has_value());
            CHECK_FALSE(cache.Contains(evicted->first));
            CHECK(cache.Size() == 3);
        }
    }
    SECTION("Clear empties the cache and it can be reused") {
        putThree(cache);
        cache.Clear();
        CHECK(cache.IsEmpty());
        CHECK_FALSE(cache.Contains(1));

        putThree(cache);
        CHECK(cache.Size() == 3);
    }
    SECTION("Get returns a copy") {
        cache.Put(1, "a");
        auto value = cache.Get(1);
        value->append("!");
        CHECK(cache.Get(1) == "a");
    }
}

TEST_CASE("CacheMap with capacity one keeps only the latest key", "[CacheMap]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::MRU, EvictionPolicy::FIFO,
                           EvictionPolicy::LIFO, EvictionPolicy::LFU, EvictionPolicy::MFU);
    CAPTURE(policy);
    Cache cache(1, policy);

    cache.Put(1, "a");
    CHECK(cache.Put(2, "b") == Item{1, "a"});
    CHECK(cache.Get(2) == "b");
    CHECK(cache.Size() == 1);
}

TEST_CASE("CacheMap using Hash String Function", "[CacheMap]") {
    JrCache::CacheMap<size_t, size_t> cache(50);
    cache.Put(JrCache::HashString("sum", 5, 5), 10);
    CHECK(cache.Get(JrCache::HashString("sum", 5, 5)) == 10);
    CHECK_FALSE(cache.Contains(JrCache::HashString("sum", 1, 1)));
    CHECK_FALSE(cache.Remove(JrCache::HashString("sum", 1, 1)).has_value());

    auto out = cache.Remove(JrCache::HashString("sum", 5, 5));
    CHECK(out.has_value());
    CHECK(out->first == JrCache::HashString("sum", 5, 5));
    CHECK(out->second == 10);
}

TEST_CASE("CacheMap using Hash Function with Hash", "[CacheMap]") {
    JrCache::CacheMap<size_t, size_t> cache(50);
    cache.Put(JrCache::Hash("sum", 5, 5), 10);
    CHECK(cache.Get(JrCache::Hash("sum", 5, 5)) == 10);
    CHECK_FALSE(cache.Contains(JrCache::Hash("sum", 1, 1)));
    CHECK_FALSE(cache.Remove(JrCache::Hash("sum", 1, 1)).has_value());

    auto out = cache.Remove(JrCache::Hash("sum", 5, 5));
    CHECK(out.has_value());
    CHECK(out->first == JrCache::Hash("sum", 5, 5));
    CHECK(out->second == 10);
}

// ---- Eviction order -----------------------------------------------------------

TEST_CASE("CacheMap LRU evicts the least recently used key", "[CacheMap][LRU]") {
    Cache cache(3, EvictionPolicy::LRU);
    putThree(cache);

    SECTION("Get counts as a use") {
        (void)cache.Get(1);
        CHECK(cache.Put(4, "d") == Item{2, "b"});
        CHECK(cache.Put(5, "e") == Item{3, "c"});
        CHECK(cache.Put(6, "f") == Item{1, "a"});
    }
    SECTION("Put on an existing key counts as a use") {
        cache.Put(1, "a2");
        CHECK(cache.Put(4, "d") == Item{2, "b"});
    }
    SECTION("Contains does not count as a use") {
        CHECK(cache.Contains(1));
        CHECK(cache.Put(4, "d") == Item{1, "a"});
    }
}

TEST_CASE("CacheMap MRU evicts the most recently used key", "[CacheMap][MRU]") {
    Cache cache(3, EvictionPolicy::MRU);
    putThree(cache);

    SECTION("without any Get, the newest key goes first") {
        CHECK(cache.Put(4, "d") == Item{3, "c"});
    }
    SECTION("Get counts as a use") {
        (void)cache.Get(1);
        CHECK(cache.Put(4, "d") == Item{1, "a"});
    }
    SECTION("Put on an existing key counts as a use") {
        cache.Put(2, "b2");
        CHECK(cache.Put(4, "d") == Item{2, "b2"});
    }
}

TEST_CASE("CacheMap FIFO evicts the key inserted longest ago", "[CacheMap][FIFO]") {
    Cache cache(3, EvictionPolicy::FIFO);
    putThree(cache);

    // Neither reads nor updates change the insertion order.
    (void)cache.Get(1);
    cache.Put(1, "a2");

    CHECK(cache.Put(4, "d") == Item{1, "a2"});
    CHECK(cache.Put(5, "e") == Item{2, "b"});
    CHECK(cache.Put(6, "f") == Item{3, "c"});
}

TEST_CASE("CacheMap LIFO evicts the key inserted most recently", "[CacheMap][LIFO]") {
    Cache cache(3, EvictionPolicy::LIFO);
    putThree(cache);

    (void)cache.Get(1);
    cache.Put(2, "b2");

    CHECK(cache.Put(4, "d") == Item{3, "c"});
    CHECK(cache.Put(5, "e") == Item{4, "d"});
    CHECK(cache.Get(1) == "a");
    CHECK(cache.Get(2) == "b2");
}

TEST_CASE("CacheMap LFU evicts the least frequently used key", "[CacheMap][LFU]") {
    Cache cache(3, EvictionPolicy::LFU);
    putThree(cache);

    // Uses: 1 -> 3, 2 -> 1, 3 -> 2.
    (void)cache.Get(1);
    cache.Put(1, "a2");
    (void)cache.Get(3);

    CHECK(cache.Put(4, "d") == Item{2, "b"});
    // 4 is new with a single use, so it is now the least used.
    CHECK(cache.Put(5, "e") == Item{4, "d"});
    CHECK(cache.Get(1) == "a2");
    CHECK(cache.Get(3) == "c");
}

TEST_CASE("CacheMap MFU evicts the most frequently used key", "[CacheMap][MFU]") {
    Cache cache(3, EvictionPolicy::MFU);
    putThree(cache);

    // Uses: 1 -> 3, 2 -> 1, 3 -> 2.
    (void)cache.Get(1);
    cache.Put(1, "a2");
    (void)cache.Get(3);

    CHECK(cache.Put(4, "d") == Item{1, "a2"});
    CHECK(cache.Put(5, "e") == Item{3, "c"});
    CHECK(cache.Get(2) == "b");
}

TEST_CASE("CacheMap LFU forgets the use count of a removed key", "[CacheMap][LFU]") {
    Cache cache(2, EvictionPolicy::LFU);
    cache.Put(1, "a");
    (void)cache.Get(1);
    (void)cache.Get(1);
    cache.Put(2, "b");
    (void)cache.Get(2);

    // 1 comes back with a fresh count of one, below 2's count of two.
    CHECK(cache.Remove(1) == Item{1, "a"});
    cache.Put(1, "a");
    CHECK(cache.Put(3, "c") == Item{1, "a"});
}

// ---- Locking --------------------------------------------------------------------

TEST_CASE("CacheMap without locking behaves the same", "[CacheMap]") {
    Cache cache(2, EvictionPolicy::LRU, false);
    cache.Put(1, "a");
    cache.Put(2, "b");
    (void)cache.Get(1);

    CHECK(cache.Put(3, "c") == Item{2, "b"});
    CHECK(cache.Size() == 2);
}

TEST_CASE("CacheMap with locking stays within capacity across threads",
          "[CacheMap][threads]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::MRU, EvictionPolicy::FIFO,
                           EvictionPolicy::LIFO, EvictionPolicy::LFU, EvictionPolicy::MFU);
    CAPTURE(policy);
    constexpr std::size_t capacity = 64;
    constexpr int thread_count = 4;
    constexpr int operations_per_thread = 2000;

    Cache cache(capacity, policy);
    // Catch2 assertions are not thread-safe, so the workers only count wrong values.
    std::atomic<int> wrong_values{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&cache, &wrong_values, t] {
            for (int i = 0; i < operations_per_thread; ++i) {
                const int key = (t * operations_per_thread + i) % 200;
                cache.Put(key, std::to_string(key));
                auto value = cache.Get(key / 2);
                if (value && *value != std::to_string(key / 2)) {
                    ++wrong_values;
                }
                if (i % 5 == 0) {
                    cache.Remove(key / 3);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(wrong_values == 0);
    CHECK(cache.Size() <= capacity);
    std::size_t found = 0;
    for (int key = 0; key < 200; ++key) {
        if (cache.Contains(key)) {
            ++found;
            CHECK(cache.Get(key) == std::to_string(key));
        }
    }
    CHECK(found == cache.Size());
}

// ---- Reading values without copying ---------------------------------------------

TEST_CASE("CacheMap GetPtr returns the cached value itself", "[CacheMap][GetPtr]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::MRU, EvictionPolicy::FIFO,
                           EvictionPolicy::LIFO, EvictionPolicy::LFU, EvictionPolicy::MFU);
    CAPTURE(policy);
    Cache cache(3, policy);
    cache.Put(1, "a");
    cache.Put(2, "b");

    SECTION("nullptr on a miss") {
        CHECK(cache.GetPtr(9) == nullptr);
    }
    SECTION("the value can be read and changed in place") {
        std::string* value = cache.GetPtr(1);
        REQUIRE(value != nullptr);
        CHECK(*value == "a");
        value->append("!");
        CHECK(cache.Get(1) == "a!");
    }
    SECTION("the pointer stays valid while the entry is cached") {
        std::string* value = cache.GetPtr(1);
        for (int i = 0; i < 5; ++i) {
            (void)cache.Get(1); // under LFU/MFU each use moves the entry to another bucket
            (void)cache.Get(2);
        }
        cache.Put(1, "a2"); // same key: updated in place
        CHECK(cache.GetPtr(1) == value);
        CHECK(*value == "a2");
    }
}

TEST_CASE("CacheMap GetPtr and Visit count as uses", "[CacheMap][GetPtr]") {
    SECTION("LRU") {
        Cache cache(2, EvictionPolicy::LRU);
        cache.Put(1, "a");
        cache.Put(2, "b");
        (void)cache.GetPtr(1);
        CHECK(cache.Put(3, "c") == Item{2, "b"});
        cache.Visit(1, [](std::string&) {});
        CHECK(cache.Put(4, "d") == Item{3, "c"});
    }
    SECTION("LFU") {
        Cache cache(2, EvictionPolicy::LFU);
        cache.Put(1, "a");
        cache.Put(2, "b");
        (void)cache.GetPtr(1);
        cache.Visit(1, [](std::string&) {});
        (void)cache.GetPtr(2);
        // Uses: 1 -> 3, 2 -> 2.
        CHECK(cache.Put(3, "c") == Item{2, "b"});
    }
}

TEST_CASE("CacheMap Visit runs on the cached value", "[CacheMap][Visit]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::FIFO, EvictionPolicy::LFU);
    CAPTURE(policy);
    Cache cache(3, policy);
    cache.Put(1, "a");

    std::string seen;
    CHECK(cache.Visit(1, [&](std::string& value) {
        seen = value;
        value = "changed";
    }));
    CHECK(seen == "a");
    CHECK(cache.Get(1) == "changed");

    bool called = false;
    CHECK_FALSE(cache.Visit(9, [&](std::string&) { called = true; }));
    CHECK_FALSE(called);
}

TEST_CASE("CacheMap Visit is safe across threads", "[CacheMap][Visit][threads]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::LFU);
    CAPTURE(policy);
    CacheMap<int, std::vector<int>> cache(32, policy);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, &wrong, t] {
            for (int i = 0; i < 3000; ++i) {
                const int key = (t * 7 + i) % 64;
                cache.Put(key, std::vector<int>(8, key));
                cache.Visit(key / 2, [&](std::vector<int>& value) {
                    for (int v : value) {
                        if (v != key / 2) ++wrong;
                    }
                });
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(wrong == 0);
    CHECK(cache.Size() <= 32);
}
