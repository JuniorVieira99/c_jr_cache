#include "cache_buckets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using JrCache::bucket;
using JrCache::BucketMap;
using Bucket = bucket<int, std::string>;
using Map = BucketMap<int, std::string>;
using Item = std::pair<int, std::string>;
using Items = std::vector<Item>;

// ---- bucket -----------------------------------------------------------------

TEST_CASE("bucket starts empty", "[bucket]") {
    Bucket b;

    CHECK(b.IsEmpty());
    CHECK(b.Size() == 0);
    CHECK_FALSE(b.Contains(1));
    CHECK_FALSE(b.Get(1).has_value());
    CHECK_FALSE(b.GetAny().has_value());
    CHECK_FALSE(b.GetAll().has_value());
}

TEST_CASE("bucket stores, overwrites and removes values", "[bucket]") {
    Bucket b;
    b.Put(1, "a");
    b.Put(2, "b");

    CHECK(b.Size() == 2);
    CHECK(b.Contains(1));
    CHECK(b.Get(1) == std::pair<int, std::string>{1, "a"});

    b.Put(1, "a2");
    CHECK(b.Size() == 2);
    CHECK(b.Get(1) == std::pair<int, std::string>{1, "a2"});

    CHECK(b.Remove(1) == Item{1, "a2"});
    CHECK_FALSE(b.Remove(99).has_value());
    CHECK(b.Size() == 1);
    CHECK_FALSE(b.Contains(1));

    b.Clear();
    CHECK(b.IsEmpty());
}

TEST_CASE("bucket GetAny returns one of the stored items", "[bucket]") {
    Bucket b;
    b.Put(1, "a");
    b.Put(2, "b");

    auto item = b.GetAny();

    REQUIRE(item.has_value());
    CHECK(b.Get(item->first) == item);
}

TEST_CASE("bucket bulk operations", "[bucket]") {
    Bucket b;
    b.PutBulk({{1, "a"}, {2, "b"}, {3, "c"}, {1, "a2"}});

    CHECK(b.Size() == 3);
    CHECK(b.Get(1)->second == "a2");

    auto all = b.GetAll();
    REQUIRE(all.has_value());
    std::sort(all->begin(), all->end());
    CHECK(*all == std::vector<std::pair<int, std::string>>{{1, "a2"}, {2, "b"}, {3, "c"}});

    CHECK(b.RemoveBulk({3, 99, 1}) == Items{{3, "c"}, {1, "a2"}});
    CHECK(b.Size() == 1);
    CHECK(b.Contains(2));
}

TEST_CASE("bucket RemoveAny and RemoveAll hand back what they removed", "[bucket]") {
    Bucket b;
    CHECK_FALSE(b.RemoveAny().has_value());
    CHECK(b.RemoveAll().empty());

    b.PutBulk({{1, "a"}, {2, "b"}, {3, "c"}});

    auto any = b.RemoveAny();
    REQUIRE(any.has_value());
    CHECK_FALSE(b.Contains(any->first));
    CHECK(b.Size() == 2);

    auto rest = b.RemoveAll();
    rest.push_back(*any);
    std::sort(rest.begin(), rest.end());
    CHECK(rest == Items{{1, "a"}, {2, "b"}, {3, "c"}});
    CHECK(b.IsEmpty());
}

// ---- BucketMap --------------------------------------------------------------

TEST_CASE("BucketMap starts empty", "[BucketMap]") {
    Map map;

    CHECK(map.Size() == 0);
    CHECK_FALSE(map.GetValue(1, 0).has_value());
    CHECK_FALSE(map.GetAny(0).has_value());
}

TEST_CASE("BucketMap keeps buckets separate", "[BucketMap]") {
    Map map;
    map.PutValue(1, "a in 0", 0);
    map.PutValue(1, "a in 5", 5);
    map.PutValue(2, "b in 5", 5);

    CHECK(map.Size() == 2);
    CHECK(map.GetValue(1, 0) == "a in 0");
    CHECK(map.GetValue(1, 5) == "a in 5");
    CHECK(map.GetValue(2, 5) == "b in 5");
    CHECK_FALSE(map.GetValue(2, 0).has_value());
    CHECK_FALSE(map.GetValue(1, 7).has_value());
}

TEST_CASE("BucketMap PutValue overwrites an existing key", "[BucketMap]") {
    Map map;
    map.PutValue(1, "a", 0);
    map.PutValue(1, "a2", 0);

    CHECK(map.Size() == 1);
    CHECK(map.GetValue(1, 0) == "a2");
}

TEST_CASE("BucketMap GetAny returns a value from the requested bucket", "[BucketMap]") {
    Map map;
    map.PutValue(1, "a", 0);
    map.PutValue(2, "b", 0);
    map.PutValue(3, "c", 1);

    auto any = map.GetAny(0);

    REQUIRE(any.has_value());
    CHECK((any == "a" || any == "b"));
    CHECK(map.GetAny(1) == "c");
    CHECK_FALSE(map.GetAny(2).has_value());
}

TEST_CASE("BucketMap PutBatch", "[BucketMap]") {
    Map map;

    SECTION("adds every item to one bucket") {
        map.PutBatch({{1, "a"}, {2, "b"}}, 3);
        CHECK(map.Size() == 1);
        CHECK(map.GetValue(1, 3) == "a");
        CHECK(map.GetValue(2, 3) == "b");
    }
    SECTION("an empty batch does not create a bucket") {
        map.PutBatch({}, 3);
        CHECK(map.Size() == 0);
    }
}

TEST_CASE("BucketMap removals drop buckets once they are empty", "[BucketMap]") {
    Map map;
    map.PutValue(1, "a", 0);
    map.PutValue(2, "b", 0);
    map.PutValue(3, "c", 1);
    REQUIRE(map.Size() == 2);

    SECTION("RemoveValue") {
        CHECK(map.RemoveValue(1, 0) == Item{1, "a"});
        CHECK(map.Size() == 2);
        CHECK_FALSE(map.GetValue(1, 0).has_value());
        CHECK(map.GetValue(2, 0) == "b");
        CHECK_FALSE(map.RemoveValue(1, 0).has_value());

        CHECK(map.RemoveValue(2, 0) == Item{2, "b"});
        CHECK(map.Size() == 1);
    }
    SECTION("RemoveBatch") {
        CHECK(map.RemoveBatch({1, 99}, 0) == Items{{1, "a"}});
        CHECK(map.Size() == 2);

        CHECK(map.RemoveBatch({2}, 0) == Items{{2, "b"}});
        CHECK(map.Size() == 1);
        CHECK(map.GetValue(3, 1) == "c");
    }
    SECTION("RemoveAny") {
        auto first = map.RemoveAny(0);
        REQUIRE(first.has_value());
        CHECK(map.Size() == 2);
        CHECK(map.GetAny(0).has_value());

        auto second = map.RemoveAny(0);
        REQUIRE(second.has_value());
        CHECK(map.Size() == 1);
        CHECK_FALSE(map.GetAny(0).has_value());

        Items removed{*first, *second};
        std::sort(removed.begin(), removed.end());
        CHECK(removed == Items{{1, "a"}, {2, "b"}});
    }
    SECTION("RemoveBucket drops a bucket with items in it") {
        auto removed = map.RemoveBucket(0);
        std::sort(removed.begin(), removed.end());
        CHECK(removed == Items{{1, "a"}, {2, "b"}});
        CHECK(map.Size() == 1);
        CHECK_FALSE(map.GetValue(1, 0).has_value());
    }
    SECTION("Clear drops every bucket") {
        map.Clear();
        CHECK(map.Size() == 0);
        CHECK_FALSE(map.GetValue(3, 1).has_value());
    }
}

TEST_CASE("BucketMap LowestIndex and HighestIndex track non-empty buckets", "[BucketMap]") {
    Map map;
    CHECK_FALSE(map.LowestIndex().has_value());
    CHECK_FALSE(map.HighestIndex().has_value());

    map.PutValue(1, "a", 4);
    map.PutValue(2, "b", 2);
    map.PutValue(3, "c", 9);
    CHECK(map.LowestIndex() == 2u);
    CHECK(map.HighestIndex() == 9u);

    map.RemoveValue(2, 2);
    map.RemoveAny(9);
    CHECK(map.LowestIndex() == 4u);
    CHECK(map.HighestIndex() == 4u);
}

TEST_CASE("BucketMap operations on missing buckets do nothing", "[BucketMap]") {
    Map map;
    map.PutValue(1, "a", 0);

    CHECK_FALSE(map.RemoveValue(1, 42).has_value());
    CHECK(map.RemoveBatch({1}, 42).empty());
    CHECK_FALSE(map.RemoveAny(42).has_value());
    CHECK(map.RemoveBucket(42).empty());
    CHECK_FALSE(map.RemoveValue(99, 0).has_value());

    CHECK(map.Size() == 1);
    CHECK(map.GetValue(1, 0) == "a");
}

TEST_CASE("BucketMap with locking stays consistent across threads", "[BucketMap][threads]") {
    constexpr int thread_count = 4;
    constexpr int puts_per_thread = 2000;
    constexpr std::size_t bucket_count = 8;

    Map map(true);
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&map, t] {
            for (int i = 0; i < puts_per_thread; ++i) {
                const int key = t * puts_per_thread + i;
                map.PutValue(key, "v", static_cast<std::size_t>(key) % bucket_count);
                if (i % 2 == 1) {
                    map.RemoveAny(static_cast<std::size_t>(t));
                }
                (void)map.GetAny(static_cast<std::size_t>(i) % bucket_count);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(map.Size() <= bucket_count);
    for (std::size_t index = 0; index < bucket_count; ++index) {
        map.RemoveBucket(index);
    }
    CHECK(map.Size() == 0);
}

TEST_CASE("BucketMap MoveValue moves a key without copying its value", "[BucketMap]") {
    Map map;
    map.PutValue(1, "a", 1);
    map.PutValue(2, "b", 1);

    SECTION("to another bucket, keeping the value's address") {
        // Read the address through a move to the same bucket, which changes nothing.
        std::string* before = map.MoveValue(1, 1, 1);
        REQUIRE(before != nullptr);
        std::string* after = map.MoveValue(1, 1, 2);
        CHECK(after == before);
        CHECK(*after == "a");
        CHECK_FALSE(map.GetValue(1, 1).has_value());
        CHECK(map.GetValue(1, 2) == "a");
        CHECK(map.Size() == 2);
    }
    SECTION("the emptied bucket is dropped") {
        map.MoveValue(1, 1, 2);
        map.MoveValue(2, 1, 2);
        CHECK(map.Size() == 1);
        CHECK(map.LowestIndex() == 2u);
    }
    SECTION("a key already in the target bucket gets the moved value") {
        map.PutValue(1, "old", 3);
        CHECK(*map.MoveValue(1, 1, 3) == "a");
        CHECK(map.GetValue(1, 3) == "a");
        CHECK_FALSE(map.GetValue(1, 1).has_value());
    }
    SECTION("a missing key or bucket changes nothing") {
        CHECK(map.MoveValue(9, 1, 2) == nullptr);
        CHECK(map.MoveValue(1, 7, 2) == nullptr);
        CHECK(map.Size() == 1);
        CHECK_FALSE(map.HighestIndex() == 2u);
    }
}
