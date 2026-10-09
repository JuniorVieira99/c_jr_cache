# JrCache

A header-only, multi-purpose in-memory cache for C++20 applications.

- **`CacheMap`**: a fixed-capacity key-value cache with six eviction policies (LRU, MRU, FIFO, LIFO, LFU, MFU).
- **`ShardedMap`**: splits a cache into independently locked shards, so threads working on different keys don't wait for each other.
- **Memoization helpers**: `Hash` and `HashString` turn a function name and its arguments into a cache key.
- **Thread-safe by default.** Locking can be turned off when a cache is only used from one thread.
- The lower-level containers the caches are built from, `LinkedList` and `BucketMap`, can also be used on their own.

## Requirements

- A C++20 compiler (tested with clang-cl, Clang and GCC).
- CMake 3.25 or newer, if you build with CMake.
- [nlohmann/json](https://github.com/nlohmann/json) for saving and loading. Version 3.12.0 is bundled in `third_party/`, so there is nothing to install.
- To run the tests: [Catch2 v3](https://github.com/catchorg/Catch2) installed through [vcpkg](https://vcpkg.io).

## Adding JrCache to your project

JrCache is header-only, so there is nothing to compile. Either copy the `headers/` and `third_party/` folders into your project and add both to your include path, or add the repository with CMake:

```cmake
add_subdirectory(external/jr_cache)
target_link_libraries(your_app PRIVATE JrCache::JrCache)
```

Linking `JrCache::JrCache` adds both include paths and requires C++20. Include the header you need:

| Header | Contents |
|---|---|
| `cache_map.hpp` | `CacheMap` |
| `cache_sharded_map.hpp` | `ShardedMap` |
| `utilities.hpp` | `EvictionPolicy`, `Hash`, `HashString` |
| `cache_serialization.hpp` | `JsonSerializable`, JSON support for `EvictionPolicy` |
| `cache_queue.hpp` | `LinkedList` |
| `cache_buckets.hpp` | `BucketMap`, `bucket` |

Everything is in the `JrCache` namespace.

## Quick start

```cpp
#include "cache_map.hpp"

#include <iostream>
#include <string>

int main() {
    // Holds at most 2 entries and evicts the least recently used one when full.
    JrCache::CacheMap<int, std::string> cache(2, JrCache::EvictionPolicy::LRU);

    cache.Put(1, "one");
    cache.Put(2, "two");
    (void)cache.Get(1);                // 1 is now the most recently used

    auto evicted = cache.Put(3, "three");
    if (evicted) {
        std::cout << "evicted " << evicted->first << '\n';  // evicted 2
    }

    if (auto value = cache.Get(1)) {
        std::cout << *value << '\n';                        // one
    }
}
```

### `CacheMap` API

```cpp
CacheMap(std::size_t capacity,
         EvictionPolicy policy = EvictionPolicy::LRU,
         bool threadSafe = true);
```

| Method | Description |
|---|---|
| `std::optional<V> Get(const K&)` | Returns a copy of the value, or `nullopt` if the key isn't cached. Counts as a use. |
| `V* GetPtr(const K&)` | Returns a pointer to the cached value itself, or `nullptr`. No copy is made, and the value can be changed through it. Counts as a use. See below for how long the pointer stays valid. |
| `bool Visit(const K&, F visitor)` | Calls `visitor(value)` with the cached value itself while the cache is locked. Returns `false` if the key isn't cached. Counts as a use. |
| `std::optional<std::pair<K, V>> Put(const K&, const V&)` | Inserts the key or replaces its value. Returns the entry **evicted** to make room, or `nullopt` if nothing was evicted. |
| `std::optional<std::pair<K, V>> Remove(const K&)` | Removes the key and returns its key-value pair, or `nullopt` if it wasn't cached. |
| `bool Contains(const K&) const` | Checks for a key without counting as a use. |
| `void Clear()` | Removes every entry. |
| `std::size_t Size() const` / `bool IsEmpty() const` | Current number of entries. |
| `std::size_t Capacity() const` | Maximum number of entries. |
| `EvictionPolicy GetEvictionPolicy() const` | The policy chosen at construction. |

The constructor throws `std::invalid_argument` if `capacity` is 0. Keys must work with `std::hash` and `operator==`, and both keys and values must be copyable.

## Eviction policies

When a new key is added to a full cache, one entry is evicted first:

| Policy | Evicts |
|---|---|
| `LRU` | The least recently used entry. `Get` and `Put` both count as uses. |
| `MRU` | The most recently used entry. |
| `FIFO` | The entry inserted longest ago. Reads and updates don't change the order. |
| `LIFO` | The entry inserted most recently. |
| `LFU` | The entry with the fewest `Get` and `Put` calls. |
| `MFU` | The entry with the most `Get` and `Put` calls. |

Replacing the value of a key that is already cached never evicts anything. Ties between LFU or MFU candidates are broken arbitrarily. Removing a key resets its use count.

### Copy, pointer or visitor?

`Get`, `GetPtr` and `Visit` all count as a use, but they differ in what you get back:

```cpp
JrCache::CacheMap<int, std::vector<double>> cache(100);

// A copy: always safe, but copies the whole vector.
if (auto copy = cache.Get(1)) { /* use *copy */ }

// The value itself: no copy. Valid until the entry leaves the cache.
if (std::vector<double>* values = cache.GetPtr(1)) {
    values->push_back(0.5);  // changes the cached value
}

// The value itself, used while the cache is locked: no copy, and safe with other threads.
cache.Visit(1, [](std::vector<double>& values) { values.push_back(0.5); });
```

- **How long a `GetPtr` pointer stays valid:** until the entry leaves the cache, through `Remove`, `Clear`, `FromJson`, or eviction when a `Put` adds another key. Using the key again or `Put`ting a new value for it doesn't move the value, under every policy.
- **`GetPtr` and threads:** the cache's lock isn't held while you use the pointer. If other threads may change the cache, use `Visit` or `Get` instead.
- **Inside `Visit`:** the visitor must not call the same cache, and should be quick, since other threads wait for it.

## Sharded cache

`ShardedMap` has the same API as `CacheMap`, but spreads keys across several `CacheMap` shards, each with its own lock. Threads that use keys in different shards don't block each other.

```cpp
#include "cache_sharded_map.hpp"

#include <string>

// 8 shards of up to 1000 entries each: 8000 entries in total.
JrCache::ShardedMap<int, std::string> cache(8, 1000, JrCache::EvictionPolicy::LRU);
```

- **Eviction is per shard.** A full shard evicts one of its own entries, even if other shards still have room. `Put` returns the entry evicted from that key's shard.
- `Capacity()` is the total across all shards.
- While other threads are writing, `Size()` and `IsEmpty()` are approximate, because they read the shards one at a time.
- The constructor throws `std::invalid_argument` if the shard count or the per-shard capacity is 0.

## Memoization

`Hash` and `HashString` in `utilities.hpp` build a cache key from a function name and its arguments, so you can cache a function's results:

```cpp
#include "cache_map.hpp"
#include "utilities.hpp"

#include <cstddef>

JrCache::CacheMap<std::size_t, long long> memo(1000);

long long fib(int n) {
    if (n < 2) return n;
    const std::size_t key = JrCache::Hash("fib", n);
    if (auto cached = memo.Get(key)) return *cached;
    const long long result = fib(n - 1) + fib(n - 2);
    memo.Put(key, result);
    return result;
}
```

- Any argument that can be written to a `std::ostream` with `<<` can be used. Arguments are kept separate, so `Hash("sum", 1, 23)` and `Hash("sum", 12, 3)` differ, and floating-point values keep their full precision.
- `Hash` and `HashString` return different values for the same call. Use the same one for both `Put` and `Get`.
- Arguments that print the same are treated as the same: `1`, `true` and `"1"` all give the same key. This is fine as long as each function name is always called with the same argument types.
- The key is a 64-bit hash, so two different calls can, very rarely, share a key and one would get the other's result. If that is not acceptable, key the cache on the arguments themselves instead of on a hash.

## Saving and loading (JSON)

`CacheMap` and `ShardedMap` can be saved to JSON and loaded back, using [nlohmann/json](https://github.com/nlohmann/json). Loading restores the eviction state too: the LRU/MRU/FIFO/LIFO order, or the LFU/MFU use counts.

```cpp
JrCache::CacheMap<std::string, int> cache(100, JrCache::EvictionPolicy::LFU);
cache.Put("a", 1);

nlohmann::json saved = cache.ToJson();
std::string text = saved.dump();
// {"buckets":[{"index":1,"items":[{"key":"a","value":1}]}],"capacity":100,"policy":"LFU"}

JrCache::CacheMap<std::string, int> restored(100, JrCache::EvictionPolicy::LFU);
restored.FromJson(nlohmann::json::parse(text));  // replaces whatever `restored` held
```

- `ToJson` returns an `nlohmann::json`; call `dump()` on it for text. `FromJson` takes an `nlohmann::json`, so to load text, pass `nlohmann::json::parse(text)`. A plain string passed to `FromJson` is not parsed; it is treated as a JSON string value and rejected.
- `FromJson` throws `nlohmann::json::exception` if the JSON doesn't have the expected shape. It throws `std::invalid_argument` if the JSON was written with a different eviction policy, repeats a key, or has more entries than the cache can hold. If it throws, the cache is unchanged.
- `ShardedMap::FromJson` puts every key in the shard it belongs to, so JSON from a map with a different shard count can be loaded.
- Keys and values can be any type nlohmann/json can convert: numbers, `bool`, `std::string`, the standard containers, and your own types once you describe them to nlohmann/json:

```cpp
struct Point { int x, y; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y)

JrCache::CacheMap<std::string, Point> points(10);
points.Put("origin", Point{0, 0});
nlohmann::json saved = points.ToJson();
```

- nlohmann/json converts numbers without range checks: a value saved as `300` and loaded into an `unsigned char` becomes `44`. Load JSON into a cache with the same key and value types it was saved from.

Caches of types nlohmann/json can't convert still work; they just don't offer `ToJson`/`FromJson`.

`LinkedList` and `BucketMap` have their own `ToJson`/`FromJson`. Because `LinkedList` doesn't own its values, its `FromJson` takes a callback that stores each value and returns a pointer to it.

## Thread safety

| Class | Thread-safe by default | How to change it |
|---|---|---|
| `CacheMap` | Yes | `CacheMap(capacity, policy, false)` |
| `ShardedMap` | Yes, one lock per shard | Always on |
| `LinkedList` | Yes | `LinkedList(false)` |
| `BucketMap` | **No** | `BucketMap(true)` |

Each call is atomic on its own. A sequence of calls, such as a `Get` followed by a `Put`, is not: another thread can change the cache in between. A pointer from `GetPtr` isn't protected by the lock either; use `Visit` to work on a cached value while other threads use the cache.

## Lower-level containers

These are the building blocks of `CacheMap`. They are public and can be used directly.

- **`LinkedList<K, V>`** (`cache_queue.hpp`) is a doubly linked list with O(1) lookup by key, which keeps keys in order and supports insertion and removal at both ends. It stores **pointers** to values and does not own them: you keep the values alive, and the `Remove*` methods hand the pointers back.
- **`BucketMap<K, V>`** (`cache_buckets.hpp`) groups key-value pairs into numbered buckets, such as one bucket per use count. Its `Remove*` methods return the removed pairs, empty buckets are dropped automatically, and `LowestIndex()` / `HighestIndex()` find the first and last non-empty bucket.

## Building and running the tests

The CMake presets in `CMakePresets.json` target Windows with vcpkg. They expect the `VCPKG_ROOT` environment variable to point to your vcpkg folder and Catch2 to be installed for the `x64-windows` triplet:

```sh
vcpkg install catch2:x64-windows
```

Then configure, build and test in one step:

```sh
cmake --workflow --preset debug
```

or step by step:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

| Preset | Toolchain |
|---|---|
| `debug`, `release` | Ninja + clang-cl |
| `vs2026` | Visual Studio 2026 solution (MSVC). Build with `vs2026-debug` or `vs2026-release`. |

CMake options:

| Option | Default | Effect |
|---|---|---|
| `DE_BUILD_TESTS` | `ON` | Build the Catch2 tests in `tests/`. |
| `DE_WARNINGS_AS_ERRORS` | `OFF` | Make compiler warnings fail the test build. |
| `DE_BUILD_BENCHMARKS` | `OFF` | Build the benchmarks in `tests/bench/`. Downloads Google Benchmark if it isn't installed. |

### Benchmarks

`tests/bench/` compares JrCache's speed and hit rate with other header-only C++ caches. Run it with:

```sh
cmake --preset bench
cmake --build --preset bench
build/bench/tests/bench/JrCacheBench.exe
```

See [tests/bench/README.md](tests/bench/README.md) for what each benchmark measures and for results.

## Status

JrCache is at version 0.1.0 and its API may still change. The `CompressionPolicy` enum in `utilities.hpp` is defined, but compression is not implemented yet.

## License

MIT. See [LICENSE](LICENSE).

The bundled [nlohmann/json](https://github.com/nlohmann/json) header is also MIT-licensed, © Niels Lohmann. See [third_party/nlohmann/LICENSE.MIT](third_party/nlohmann/LICENSE.MIT).
