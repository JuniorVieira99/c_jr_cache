#include "cache_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using JrCache::LinkedList;
using List = LinkedList<int, std::string>;

namespace {

// Empties the list from the head and returns the values in head-to-tail order.
std::vector<std::string> drainFromTop(List& list) {
    std::vector<std::string> values;
    while (auto value = list.RemoveTop()) {
        values.push_back(**value);
    }
    return values;
}

} // namespace

TEST_CASE("LinkedList starts empty", "[LinkedList]") {
    List list;

    CHECK(list.IsEmpty());
    CHECK(list.GetSize() == 0);
    CHECK_FALSE(list.ContainsNode(1));
    CHECK_FALSE(list.GetTop().has_value());
    CHECK_FALSE(list.GetBottom().has_value());
    CHECK_FALSE(list.RemoveTop().has_value());
    CHECK_FALSE(list.RemoveBottom().has_value());
    CHECK_FALSE(list.Remove(1).has_value());
}

TEST_CASE("LinkedList with one element has the same top and bottom", "[LinkedList]") {
    List list;
    std::string a = "a";

    list.SetTop(1, &a);

    CHECK_FALSE(list.IsEmpty());
    CHECK(list.GetSize() == 1);
    CHECK(list.ContainsNode(1));
    CHECK(list.GetTop() == "a");
    CHECK(list.GetBottom() == "a");

    SECTION("removing it from the top empties the list") {
        CHECK(list.RemoveTop() == &a);
    }
    SECTION("removing it from the bottom empties the list") {
        CHECK(list.RemoveBottom() == &a);
    }
    SECTION("removing it by key empties the list") {
        CHECK(list.Remove(1) == &a);
    }

    CHECK(list.IsEmpty());
    CHECK_FALSE(list.GetTop().has_value());
    CHECK_FALSE(list.GetBottom().has_value());
}

TEST_CASE("LinkedList SetTop and SetBottom place new keys at the ends", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b", c = "c", d = "d";

    list.SetTop(1, &a);
    list.SetTop(2, &b);
    list.SetBottom(3, &c);
    list.SetBottom(4, &d);

    CHECK(list.GetSize() == 4);
    CHECK(list.GetTop() == "b");
    CHECK(list.GetBottom() == "d");
    CHECK(drainFromTop(list) == std::vector<std::string>{"b", "a", "c", "d"});
}

TEST_CASE("LinkedList setting an existing key moves it and replaces its value", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b", c = "c", replacement = "replacement";
    list.SetTop(1, &a);
    list.SetTop(2, &b);
    list.SetTop(3, &c); // 3 2 1

    SECTION("SetTop moves a key from the bottom to the top") {
        list.SetTop(1, &replacement);
        CHECK(drainFromTop(list) == std::vector<std::string>{"replacement", "c", "b"});
    }
    SECTION("SetTop moves a key from the middle to the top") {
        list.SetTop(2, &replacement);
        CHECK(drainFromTop(list) == std::vector<std::string>{"replacement", "c", "a"});
    }
    SECTION("SetBottom moves a key from the top to the bottom") {
        list.SetBottom(3, &replacement);
        CHECK(drainFromTop(list) == std::vector<std::string>{"b", "a", "replacement"});
    }
    SECTION("SetBottom moves a key from the middle to the bottom") {
        list.SetBottom(2, &replacement);
        CHECK(drainFromTop(list) == std::vector<std::string>{"c", "a", "replacement"});
    }
    SECTION("SetTop on the current top only replaces the value") {
        list.SetTop(3, &replacement);
        CHECK(drainFromTop(list) == std::vector<std::string>{"replacement", "b", "a"});
    }
}

TEST_CASE("LinkedList updating a key does not change the size", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b";
    list.SetTop(1, &a);
    list.SetTop(1, &b);
    list.SetBottom(1, &a);

    CHECK(list.GetSize() == 1);
    CHECK(list.GetTop() == "a");
}

TEST_CASE("LinkedList ignores null values", "[LinkedList]") {
    List list;
    std::string a = "a";
    list.SetTop(1, &a);

    list.SetTop(2, nullptr);
    list.SetBottom(3, nullptr);
    list.SetTop(1, nullptr);

    CHECK(list.GetSize() == 1);
    CHECK_FALSE(list.ContainsNode(2));
    CHECK_FALSE(list.ContainsNode(3));
    CHECK(list.GetTop() == "a");
}

TEST_CASE("LinkedList removes nodes and hands back the value pointer", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b", c = "c";
    list.SetBottom(1, &a);
    list.SetBottom(2, &b);
    list.SetBottom(3, &c); // 1 2 3

    SECTION("Remove takes a node out of the middle") {
        CHECK(list.Remove(2) == &b);
        CHECK_FALSE(list.ContainsNode(2));
        CHECK_FALSE(list.Remove(2).has_value());
        CHECK(drainFromTop(list) == std::vector<std::string>{"a", "c"});
    }
    SECTION("RemoveTop takes the head") {
        CHECK(list.RemoveTop() == &a);
        CHECK(list.GetTop() == "b");
        CHECK(list.GetSize() == 2);
    }
    SECTION("RemoveBottom takes the tail") {
        CHECK(list.RemoveBottom() == &c);
        CHECK(list.GetBottom() == "b");
        CHECK(list.GetSize() == 2);
    }
    SECTION("a missing key leaves the list unchanged") {
        CHECK_FALSE(list.Remove(42).has_value());
        CHECK(list.GetSize() == 3);
    }
}

TEST_CASE("LinkedList Get functions return copies", "[LinkedList]") {
    List list;
    std::string a = "a";
    list.SetTop(1, &a);

    auto top = list.GetTop();
    a = "changed";

    CHECK(top == "a");
    CHECK(list.GetTop() == "changed");
}

TEST_CASE("LinkedList SetBatchTop acts like SetTop for each item in order", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b", c = "c", d = "d";
    list.SetTop(1, &a);
    list.SetTop(2, &b); // 2 1

    list.SetBatchTop({{3, &c}, {1, &a}, {4, &d}});

    CHECK(list.GetSize() == 4);
    CHECK(drainFromTop(list) == std::vector<std::string>{"d", "a", "c", "b"});
}

TEST_CASE("LinkedList SetBatchBottom acts like SetBottom for each item in order", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b", c = "c", d = "d";
    list.SetTop(1, &a);
    list.SetTop(2, &b); // 2 1

    list.SetBatchBottom({{3, &c}, {2, &b}, {4, &d}});

    CHECK(list.GetSize() == 4);
    CHECK(drainFromTop(list) == std::vector<std::string>{"a", "c", "b", "d"});
}

TEST_CASE("LinkedList batch set handles duplicates, nulls and empty input", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b";

    SECTION("the last value for a duplicated key wins") {
        list.SetBatchTop({{1, &a}, {1, &b}});
        CHECK(list.GetSize() == 1);
        CHECK(list.GetTop() == "b");
    }
    SECTION("null values are skipped") {
        list.SetBatchBottom({{1, &a}, {2, nullptr}});
        CHECK(list.GetSize() == 1);
        CHECK_FALSE(list.ContainsNode(2));
    }
    SECTION("an empty batch does nothing") {
        list.SetBatchTop({});
        list.SetBatchBottom({});
        CHECK(list.IsEmpty());
    }
}

TEST_CASE("LinkedList GetBatch returns a result per key", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b";
    list.SetTop(1, &a);
    list.SetTop(2, &b);

    auto values = list.GetBatch({2, 99, 1});

    REQUIRE(values.size() == 3);
    CHECK(values[0] == "b");
    CHECK_FALSE(values[1].has_value());
    CHECK(values[2] == "a");
    CHECK(list.GetSize() == 2);
}

TEST_CASE("LinkedList RemoveBatch returns only the removed values", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b", c = "c";
    list.SetBottom(1, &a);
    list.SetBottom(2, &b);
    list.SetBottom(3, &c);

    auto removed = list.RemoveBatch({3, 99, 1, 3});

    CHECK(removed == std::vector<std::string*>{&c, &a});
    CHECK(list.GetSize() == 1);
    CHECK(drainFromTop(list) == std::vector<std::string>{"b"});
}

TEST_CASE("LinkedList Clear empties the list and it can be reused", "[LinkedList]") {
    List list;
    std::string a = "a", b = "b";
    list.SetTop(1, &a);
    list.SetTop(2, &b);

    list.Clear();

    CHECK(list.IsEmpty());
    CHECK_FALSE(list.ContainsNode(1));
    CHECK_FALSE(list.GetTop().has_value());

    list.SetBottom(1, &a);
    CHECK(list.GetSize() == 1);
    CHECK(list.GetTop() == "a");
}

TEST_CASE("LinkedList without locking behaves the same", "[LinkedList]") {
    List list(false);
    std::string a = "a", b = "b";

    list.SetTop(1, &a);
    list.SetBottom(2, &b);

    CHECK(list.GetSize() == 2);
    CHECK(list.RemoveBottom() == &b);
    CHECK(list.RemoveTop() == &a);
    CHECK(list.IsEmpty());
}

TEST_CASE("LinkedList with locking stays consistent across threads", "[LinkedList][threads]") {
    constexpr int thread_count = 4;
    constexpr int keys_per_thread = 1000;

    List list;
    std::vector<std::string> values(thread_count * keys_per_thread, "v");
    std::vector<int> all_keys;
    for (int key = 0; key < thread_count * keys_per_thread; ++key) {
        all_keys.push_back(key);
    }

    std::vector<std::thread> threads;
    std::vector<int> removed_per_thread(thread_count, 0);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < keys_per_thread; ++i) {
                int key = t * keys_per_thread + i;
                list.SetTop(key, &values[key]);
                if (i % 3 == 0 && list.RemoveBottom()) {
                    ++removed_per_thread[t];
                }
                (void)list.GetTop();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::uint64_t removed = 0;
    for (int count : removed_per_thread) {
        removed += count;
    }
    const std::uint64_t remaining = list.GetSize();
    CHECK(remaining + removed == all_keys.size());
    CHECK(list.RemoveBatch(all_keys).size() == remaining);
    CHECK(list.IsEmpty());
}
