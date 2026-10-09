#include "cache_buckets.hpp"
#include "cache_map.hpp"
#include "cache_queue.hpp"
#include "cache_serialization.hpp"
#include "cache_sharded_map.hpp"
#include "utilities.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using JrCache::EvictionPolicy;
using nlohmann::json;

namespace {

// A user type made serializable with nlohmann's macro.
struct Point {
    int x;
    int y;
    bool operator==(const Point&) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y)

// No serializer: caches of it must still compile, just without ToJson/FromJson.
struct Opaque {
    int value;
};

} // namespace

static_assert(JrCache::JsonSerializable<int>);
static_assert(JrCache::JsonSerializable<std::string>);
static_assert(JrCache::JsonSerializable<std::vector<double>>);
static_assert(JrCache::JsonSerializable<Point>);
static_assert(JrCache::JsonSerializable<EvictionPolicy>);
static_assert(!JrCache::JsonSerializable<Opaque>);

// ---- EvictionPolicy ----------------------------------------------------------------

TEST_CASE("EvictionPolicy converts to and from its name", "[serialization]") {
    for (auto policy : {EvictionPolicy::LRU, EvictionPolicy::LFU, EvictionPolicy::MRU,
                        EvictionPolicy::MFU, EvictionPolicy::FIFO, EvictionPolicy::LIFO}) {
        json j = policy;
        CHECK(j.get<std::string>() == JrCache::PolicyName(policy));
        CHECK(j.get<EvictionPolicy>() == policy);
    }
    CHECK_THROWS_AS(json("RANDOM").get<EvictionPolicy>(), std::invalid_argument);
    CHECK_THROWS_AS(json(3).get<EvictionPolicy>(), json::type_error);
}

// ---- LinkedList --------------------------------------------------------------------

TEST_CASE("LinkedList ToJson lists items from top to bottom", "[serialization][LinkedList]") {
    JrCache::LinkedList<int, std::string> list;
    CHECK(list.ToJson() == json::array());

    std::string a = "a", b = "b\"";
    list.SetBottom(1, &a);
    list.SetBottom(2, &b);
    CHECK(list.ToJson().dump() == R"([{"key":1,"value":"a"},{"key":2,"value":"b\""}])");
}

TEST_CASE("LinkedList FromJson stores values through the callback", "[serialization][LinkedList]") {
    using List = JrCache::LinkedList<int, std::string>;
    std::deque<std::string> storage; // deque keeps element addresses stable on push_back
    auto store = [&](const int&, std::string&& value) {
        storage.push_back(std::move(value));
        return &storage.back();
    };

    std::string x = "x";
    List list;
    list.SetTop(9, &x);
    const json items = json::parse(R"([{"key":1,"value":"a"}, {"value":"b", "key":2}])");

    SECTION("below the existing nodes") {
        list.FromJson(items, store);
        CHECK(list.ToJson().dump() ==
              R"([{"key":9,"value":"x"},{"key":1,"value":"a"},{"key":2,"value":"b"}])");
    }
    SECTION("above the existing nodes, keeping their order") {
        list.FromJson(items, store, true);
        CHECK(list.ToJson().dump() ==
              R"([{"key":1,"value":"a"},{"key":2,"value":"b"},{"key":9,"value":"x"}])");
    }
    SECTION("a list survives a round trip through text") {
        list.FromJson(items, store);
        List copy;
        copy.FromJson(json::parse(list.ToJson().dump()), store);
        CHECK(copy.ToJson() == list.ToJson());
    }
    SECTION("items the callback rejects are skipped") {
        list.FromJson(items, [&](const int& key, std::string&& value) -> std::string* {
            return key == 1 ? store(key, std::move(value)) : nullptr;
        });
        CHECK(list.GetSize() == 2);
        CHECK_FALSE(list.ContainsNode(2));
    }
    SECTION("JSON with the wrong shape adds nothing") {
        CHECK_THROWS_AS(list.FromJson(json::parse(R"([{"key":1,"value":"a"},{"key":2}])"), store),
                        json::exception);
        CHECK_THROWS_AS(list.FromJson(json::parse(R"({"key":1,"value":"a"})"), store),
                        json::exception);
        CHECK_THROWS_AS(list.FromJson(json::parse(R"([{"key":"one","value":"a"}])"), store),
                        json::exception);
        CHECK(list.GetSize() == 1);
        CHECK(storage.empty());
    }
}

// ---- BucketMap ---------------------------------------------------------------------

TEST_CASE("BucketMap ToJson and FromJson round-trip", "[serialization][BucketMap]") {
    JrCache::BucketMap<int, std::string> map;
    CHECK(map.ToJson() == json::array());

    map.PutValue(1, "a", 3);
    map.PutValue(2, "b", 1);
    CHECK(map.ToJson().dump() ==
          R"([{"index":1,"items":[{"key":2,"value":"b"}]},{"index":3,"items":[{"key":1,"value":"a"}]}])");

    JrCache::BucketMap<int, std::string> copy;
    copy.FromJson(map.ToJson());
    CHECK(copy.Size() == 2);
    CHECK(copy.GetValue(1, 3) == "a");
    CHECK(copy.GetValue(2, 1) == "b");

    SECTION("FromJson merges into existing buckets") {
        copy.FromJson(json::parse(
            R"([{"items":[{"key":1,"value":"a2"},{"key":5,"value":"e"}],"index":3}])"));
        CHECK(copy.GetValue(1, 3) == "a2");
        CHECK(copy.GetValue(5, 3) == "e");
        CHECK(copy.GetValue(2, 1) == "b");
    }
    SECTION("an empty bucket in the JSON creates nothing") {
        copy.Clear();
        copy.FromJson(json::parse(R"([{"index":4,"items":[]}])"));
        CHECK(copy.Size() == 0);
    }
    SECTION("JSON with the wrong shape adds nothing") {
        CHECK_THROWS_AS(copy.FromJson(json::parse(R"([{"index":7}])")), json::exception);
        CHECK_THROWS_AS(
            copy.FromJson(json::parse(R"([{"index":7,"items":[{"key":9,"value":"z"}]},{"index":"x","items":[]}])")),
            json::exception);
        CHECK(copy.Size() == 2);
        CHECK_FALSE(copy.GetValue(9, 7).has_value());
    }
}

// ---- CacheMap ----------------------------------------------------------------------

TEST_CASE("CacheMap round trip keeps the eviction order", "[serialization][CacheMap]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::MRU, EvictionPolicy::FIFO,
                           EvictionPolicy::LIFO, EvictionPolicy::LFU, EvictionPolicy::MFU);
    CAPTURE(policy);

    JrCache::CacheMap<int, std::string> original(4, policy);
    original.Put(1, "a");
    original.Put(2, "b");
    original.Put(3, "c");
    original.Put(4, "d");
    // Distinct use counts, so LFU and MFU have no ties: 1 -> 4, 2 -> 1, 3 -> 3, 4 -> 2.
    for (int i = 0; i < 3; ++i) (void)original.Get(1);
    for (int i = 0; i < 2; ++i) (void)original.Get(3);
    (void)original.Get(4);

    JrCache::CacheMap<int, std::string> copy(4, policy);
    copy.Put(99, "old contents are replaced");
    copy.FromJson(json::parse(original.ToJson().dump()));

    CHECK(copy.Size() == 4);
    CHECK_FALSE(copy.Contains(99));
    CHECK(copy.ToJson() == original.ToJson());
    for (int key = 10; key < 14; ++key) {
        CHECK(copy.Put(key, "new") == original.Put(key, "new"));
    }
}

TEST_CASE("CacheMap ToJson format", "[serialization][CacheMap]") {
    JrCache::CacheMap<int, int> lru(5, EvictionPolicy::LRU);
    lru.Put(1, 10);
    lru.Put(2, 20);
    CHECK(lru.ToJson() == json::parse(R"({"policy":"LRU","capacity":5,
        "entries":[{"key":2,"value":20},{"key":1,"value":10}]})"));

    JrCache::CacheMap<int, int> lfu(5, EvictionPolicy::LFU);
    lfu.Put(1, 10);
    (void)lfu.Get(1);
    CHECK(lfu.ToJson() == json::parse(R"({"policy":"LFU","capacity":5,
        "buckets":[{"index":2,"items":[{"key":1,"value":10}]}]})"));
}

TEST_CASE("CacheMap FromJson rejects JSON that does not fit", "[serialization][CacheMap]") {
    JrCache::CacheMap<int, int> cache(2, EvictionPolicy::LRU);
    cache.Put(7, 70);

    SECTION("another policy") {
        JrCache::CacheMap<int, int> lfu(2, EvictionPolicy::LFU);
        lfu.Put(1, 10);
        CHECK_THROWS_AS(cache.FromJson(lfu.ToJson()), std::invalid_argument);
    }
    SECTION("more entries than the capacity") {
        JrCache::CacheMap<int, int> bigger(3, EvictionPolicy::LRU);
        bigger.Put(1, 10);
        bigger.Put(2, 20);
        bigger.Put(3, 30);
        CHECK_THROWS_AS(cache.FromJson(bigger.ToJson()), std::invalid_argument);
    }
    SECTION("a repeated key") {
        CHECK_THROWS_AS(cache.FromJson(json::parse(
                            R"({"policy":"LRU","entries":[{"key":1,"value":1},{"key":1,"value":2}]})")),
                        std::invalid_argument);
    }
    SECTION("the wrong shape") {
        CHECK_THROWS_AS(cache.FromJson(json::parse(R"({"policy":"LRU","buckets":[]})")), json::exception);
        CHECK_THROWS_AS(cache.FromJson(json::parse(R"({"entries":[]})")), json::exception);
        CHECK_THROWS_AS(cache.FromJson(json::parse(R"([1, 2, 3])")), json::exception);
        // A string is not parsed: pass nlohmann::json::parse(text) to load text.
        CHECK_THROWS_AS(cache.FromJson(std::string(R"({"policy":"LRU","entries":[]})")),
                        json::exception);
    }

    // A failed load leaves the cache as it was.
    CHECK(cache.Size() == 1);
    CHECK(cache.Get(7) == 70);
}

TEST_CASE("CacheMap serializes user and standard types", "[serialization][CacheMap]") {
    JrCache::CacheMap<std::string, Point> points(3, EvictionPolicy::FIFO);
    points.Put("origin", Point{0, 0});
    points.Put("corner", Point{5, -5});

    JrCache::CacheMap<std::string, Point> copy(3, EvictionPolicy::FIFO);
    copy.FromJson(points.ToJson());
    CHECK(copy.Get("corner") == Point{5, -5});
    CHECK(copy.Put("third", Point{1, 1}) == std::nullopt);
    CHECK(copy.Put("fourth", Point{2, 2})->first == "origin");

    JrCache::CacheMap<int, std::vector<std::string>> lists(2, EvictionPolicy::LRU);
    lists.Put(1, {"a", "b"});
    JrCache::CacheMap<int, std::vector<std::string>> listsCopy(2, EvictionPolicy::LRU);
    listsCopy.FromJson(lists.ToJson());
    CHECK(listsCopy.Get(1) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("Caches of non-serializable values still work", "[serialization][CacheMap]") {
    JrCache::CacheMap<int, Opaque> cache(2);
    cache.Put(1, Opaque{5});
    CHECK(cache.Get(1)->value == 5);
}

// ---- ShardedMap --------------------------------------------------------------------

TEST_CASE("ShardedMap round trip", "[serialization][ShardedMap]") {
    auto policy = GENERATE(EvictionPolicy::LRU, EvictionPolicy::LFU);
    CAPTURE(policy);

    JrCache::ShardedMap<int, std::string> original(4, 50, policy);
    for (int key = 0; key < 100; ++key) {
        original.Put(key, std::to_string(key));
    }
    for (int key = 0; key < 100; key += 3) {
        (void)original.Get(key);
    }
    const json saved = json::parse(original.ToJson().dump());

    SECTION("into a map with the same shards") {
        JrCache::ShardedMap<int, std::string> copy(4, 50, policy);
        copy.Put(1000, "replaced");
        copy.FromJson(saved);
        CHECK(copy.Size() == 100);
        CHECK_FALSE(copy.Contains(1000));
        // Items inside an LFU bucket have no fixed order, so only LRU output is comparable.
        if (policy == EvictionPolicy::LRU) {
            CHECK(copy.ToJson() == saved);
        }
        for (int key = 0; key < 100; ++key) {
            CHECK(copy.Get(key) == std::to_string(key));
        }
    }
    SECTION("into a map with a different shard count") {
        JrCache::ShardedMap<int, std::string> copy(7, 40, policy);
        copy.FromJson(saved);
        CHECK(copy.Size() == 100);
        for (int key = 0; key < 100; ++key) {
            CHECK(copy.Get(key) == std::to_string(key));
        }
    }
    SECTION("into shards that are too small") {
        JrCache::ShardedMap<int, std::string> copy(2, 10, policy);
        copy.Put(1000, "kept");
        CHECK_THROWS_AS(copy.FromJson(saved), std::invalid_argument);
        CHECK(copy.Size() == 1);
        CHECK(copy.Get(1000) == "kept");
    }
    SECTION("into a map with another policy") {
        JrCache::ShardedMap<int, std::string> copy(4, 50, EvictionPolicy::FIFO);
        CHECK_THROWS_AS(copy.FromJson(saved), std::invalid_argument);
        CHECK(copy.IsEmpty());
    }
}

TEST_CASE("ShardedMap keeps LFU use counts when shards change", "[serialization][ShardedMap]") {
    JrCache::ShardedMap<int, int> original(3, 10, EvictionPolicy::LFU);
    original.Put(1, 10);
    original.Put(2, 20);
    for (int i = 0; i < 5; ++i) (void)original.Get(1);

    // One shard, so both keys meet and the least used one (2) is evicted first.
    JrCache::ShardedMap<int, int> single(1, 2, EvictionPolicy::LFU);
    single.FromJson(original.ToJson());
    CHECK(single.Put(3, 30) == std::pair<int, int>{2, 20});
    CHECK(single.Contains(1));
}

TEST_CASE("ShardedMap FromJson rejects JSON with the wrong shape", "[serialization][ShardedMap]") {
    JrCache::ShardedMap<int, int> map(2, 5, EvictionPolicy::LRU);
    map.Put(1, 10);

    CHECK_THROWS_AS(map.FromJson(json::parse(R"({"policy":"LRU"})")), json::exception);
    CHECK_THROWS_AS(map.FromJson(json::parse(R"({"policy":"LRU","shards":{}})")), json::exception);
    CHECK_THROWS_AS(
        map.FromJson(json::parse(R"({"policy":"LRU","shards":[{"policy":"LRU","entries":[{"key":1}]}]})")),
        json::exception);
    CHECK_THROWS_AS(
        map.FromJson(json::parse(R"({"policy":"LRU","shards":[{"policy":"LFU","buckets":[]}]})")),
        std::invalid_argument);
    CHECK(map.Size() == 1);
}
