#include "utilities.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using JrCache::Hash;
using JrCache::HashString;

TEST_CASE("HashString is the same for the same call", "[utilities][HashString]") {
    CHECK(HashString("sum", 5, 5) == HashString("sum", 5, 5));
    CHECK(HashString("greet", std::string("bob"), 2.5) ==
          HashString("greet", std::string("bob"), 2.5));
    CHECK(HashString("now") == HashString("now"));
}

TEST_CASE("HashString tells different calls apart", "[utilities][HashString]") {
    CHECK(HashString("sum", 5, 5) != HashString("sum", 5, 6));
    CHECK(HashString("sum", 5, 5) != HashString("mul", 5, 5));
    CHECK(HashString("sum", 1, 2) != HashString("sum", 2, 1));
    CHECK(HashString("sum", 1) != HashString("sum", 1, 1));
}

TEST_CASE("HashString does not merge neighbouring arguments", "[utilities][HashString]") {
    CHECK(HashString("sum", 1, 23) != HashString("sum", 12, 3));
    CHECK(HashString("sum1", 2) != HashString("sum", 12));
    CHECK(HashString("f", std::string("a"), std::string("bc")) !=
          HashString("f", std::string("ab"), std::string("c")));
    CHECK(HashString("f", std::string("1:a")) != HashString("f", std::string("1"), std::string("a")));
}

TEST_CASE("HashString keeps full floating-point precision", "[utilities][HashString]") {
    CHECK(HashString("f", 1.0) != HashString("f", 1.0000001));
    CHECK(HashString("f", 0.1 + 0.2) != HashString("f", 0.3));
    CHECK(HashString("f", 1.5f) == HashString("f", 1.5f));
}

TEST_CASE("Hash is the same for the same call and differs otherwise", "[utilities][Hash]") {
    CHECK(Hash("sum", 5, 5) == Hash("sum", 5, 5));
    CHECK(Hash("sum", 5, 5) != Hash("sum", 5, 6));
    CHECK(Hash("sum", 5, 5) != Hash("mul", 5, 5));
    CHECK(Hash("sum", 1, 23) != Hash("sum", 12, 3));
}
