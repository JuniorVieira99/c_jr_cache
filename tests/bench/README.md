# JrCache benchmarks

These benchmarks measure JrCache and compare it with three other header-only C++ caches on the same workloads, using [Google Benchmark](https://github.com/google/benchmark).

## Running them

```sh
cmake --preset bench
cmake --build --preset bench
build/bench/tests/bench/JrCacheBench.exe
```

The `bench` preset is a Release build with `DE_BUILD_BENCHMARKS=ON`. If CMake can't find an installed Google Benchmark, it downloads v1.9.5 (checked against its SHA-256) the first time you configure. Normal builds never download anything.

Useful flags:

```sh
# Only some benchmarks (a regular expression)
JrCacheBench.exe --benchmark_filter=Zipf/
# Repeat each benchmark and keep the median, saving everything as JSON
JrCacheBench.exe --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
    --benchmark_out=results.json --benchmark_out_format=json
```

## What is compared

| Name in results | Library | Locking |
|---|---|---|
| `JrCache.LRU`, `.LFU`, `.FIFO` | `JrCache::CacheMap` | Mutex (the default) |
| `JrCache.LRU.unlocked` | `JrCache::CacheMap` with `threadSafe = false` | None |
| `JrCache.ShardedLRU(16)` | `JrCache::ShardedMap`, 16 shards | Mutex per shard |
| `caches.LRU`, `.LFU`, `.FIFO` | [vpetrigo/caches](https://github.com/vpetrigo/caches) | Mutex, always |
| `cpp-lru-cache` | [lamerman/cpp-lru-cache](https://github.com/lamerman/cpp-lru-cache) | None |
| `lrucache11`, `lrucache11(mutex)` | [mohaps/lrucache11](https://github.com/mohaps/lrucache11) | None, or `std::mutex` |
| `unordered_map(no-eviction)` | `std::unordered_map`, which never evicts: a lower bound, not a cache | None |

The other libraries are in `third_party/`; see [third_party/README.md](third_party/README.md) for versions and licenses.

Keys are 64-bit integers, spread over the full 64-bit range. Every cache is wrapped in an adapter with the same two calls:
- `Get(key, out)` copies the value into `out` on a hit;
- `Put(key, value)` inserts or updates a key.

Each benchmark replays a key sequence generated in advance, so random number generation isn't timed.

| Benchmark | What it measures |
|---|---|
| `Put/<capacity>` | Inserting new keys into a full cache, so every call also evicts one entry. |
| `GetHit/<capacity>` | Looking up keys that are all cached, chosen at random. |
| `GetMiss/<capacity>` | Looking up keys that aren't cached. |
| `GetHit.string/10000` | `GetHit` with 32-character `std::string` values, long enough that copying one allocates memory. `JrCache.LRU.GetPtr` reads through `GetPtr` and copies nothing; every other entry copies the value. |
| `Zipf` | A read-through cache: `Get`, then `Put` on a miss. Keys come from 100,000 possible keys with a Zipf distribution (skew 0.99, so a few keys are very popular), and the cache holds 10,000. Also reports the hit rate. |
| `ZipfMT` | `Zipf` with 1 to 16 threads sharing one cache. Only thread-safe caches are included. |

## Results

Measured on 2026-10-08:
- **CPU:** Intel Core Ultra 9 275HX (24 cores: 8 performance, 16 efficiency), on Windows 11.
- **Compiler:** clang-cl 22.1.4, `/O2`.
- **Method:** each number is the median of 3 runs. The raw data is in [results/results.json](results/results.json).

Lower is better for time. Numbers from another machine or compiler will differ, but the comparisons between libraries are what matter.

### LRU, single thread (ns per operation)

| | GetHit 1k | GetHit 100k | GetMiss 1k | Put 1k | Put 100k | Zipf | Zipf hit rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| JrCache.LRU | 23 | 61 | 19 | 107 | 174 | 55 | 72.5% |
| JrCache.LRU.unlocked | 17 | 40 | 16 | 102 | 166 | 47 | 72.5% |
| caches.LRU | 43* | 85 | 18 | 139 | 214 | 75 | 72.5% |
| cpp-lru-cache | 12 | 31 | 11 | 65 | 98 | 31 | 72.5% |
| lrucache11 | 10 | 26 | 12 | 65 | 97 | 29 | 72.5% |
| unordered_map (reference) | 8.4 | 14 | 12 | – | – | – | – |

\* Varied 27% between repetitions in this run; earlier runs measured 36–37 ns.

### LFU and FIFO, single thread (ns per operation)

| | GetHit 1k | GetHit 100k | Put 1k | Put 100k | Zipf | Zipf hit rate |
|---|---:|---:|---:|---:|---:|---:|
| JrCache.LFU | **106** | **129** | **91** | 233 | **159** | **78.5%** |
| caches.LFU | 140 | 382 | 138 | **174** | 174 | 73.3% |
| JrCache.FIFO | **17** | **28** | **105** | **170** | **53** | 68.9% |
| caches.FIFO | 31 | 45 | 137 | 213 | 70 | 68.9% |

### String values (`GetHit.string/10000`, ns per operation)

| JrCache.LRU (`Get`, copy) | JrCache.LRU (`GetPtr`, no copy) | caches.LRU | cpp-lru-cache | lrucache11 |
|---:|---:|---:|---:|---:|
| 54 | 26 | 39 | 14 | 46 |

### Threads sharing one cache (`ZipfMT`, total million operations per second, higher is better)

| Threads | 1 | 2 | 4 | 8 | 16 |
|---|---:|---:|---:|---:|---:|
| JrCache.LRU | 17.8 | 14.1 | 15.1 | 9.2 | 4.9 |
| JrCache.ShardedLRU(16) | 13.8 | 10.8 | 11.2 | **13.1** | **12.2** |
| caches.LRU | 13.6 | 12.8 | 10.8 | 6.9 | 3.6 |
| lrucache11(mutex) | **25.8** | **23.2** | **18.9** | 12.6 | 5.7 |

The thread results vary more between runs (up to about 10%) than most single-thread ones (mostly under 2%).

## What the results show

**Where JrCache does well**
- **LRU is faster than `caches.LRU` everywhere.** Inserts take 107 against 139 ns, and the Zipf workload 55 against 75 ns.
- **The sharded cache scales best.** At 8 and 16 threads, `ShardedMap` handles the most operations per second of every thread-safe cache tested: 12.2 million at 16 threads, against 5.7 million for `lrucache11(mutex)` and 4.9 million for a single `CacheMap`.
- **LFU is now faster than `caches.LFU`, and keeps the most useful entries.** In the Zipf test it is faster (159 against 174 ns) and its hit rate is higher (78.5% against 73.3%). When the cache fronts something slow, such as a database or a network call, 5 more hits per 100 requests matters more than nanoseconds. Its hits take 106 ns at 1,000 entries and 129 ns at 100,000, against 140 and 382 ns for `caches.LFU`. A use moves the stored entry to its next bucket without copying the value.
- **FIFO is the fastest FIFO here.** Lookups take half to two-thirds the time of `caches.FIFO`, and inserts are about 20–25% faster.
- **`GetPtr` removes the cost of copying.** With string values, a hit through `GetPtr` takes 26 ns, against 54 ns through `Get`, which copies the string.

**Where JrCache is slower**
- **LRU hits are still about twice as slow as the minimal LRU libraries.** A hit takes 23 ns (17 without the lock), against 10–12 ns for `cpp-lru-cache` and `lrucache11`. A hit does two hash lookups, one in `CacheMap`'s map and one in the list's. The minimal libraries do one.
- **Single-threaded, `lrucache11(mutex)` is still faster.** It wins up to 4 threads, and JrCache's sharded cache overtakes it from 8.
- **LFU inserts slow down with large caches.** At 1,000 entries an insert into a full LFU cache is faster than `caches.LFU` (91 against 138 ns). At 100,000 it is slower (233 against 174 ns). Each such insert erases from and inserts into two hash tables (the bucket and the use-count map), with an allocation for each insert. That likely costs more once those tables no longer fit in the CPU cache.
- **The lock costs more in a big cache.** Turning off the mutex saves 3–9 ns per call in most benchmarks, but about 20 ns on hits in the 100,000-entry cache.

**Other things to keep in mind**
- **`lrucache11` and `cpp-lru-cache` are not thread-safe** unless you add a lock, and `cpp-lru-cache`'s `get()` throws on a miss. Its 14 ns string hit comes from returning a reference, like `GetPtr`.
- **`caches` keeps each value in its own `std::shared_ptr`.** That makes its lookups cheap but costs an allocation per insert.
- **Sharding doesn't speed up a single thread.** With one thread, `ShardedLRU(16)` is slower than one `CacheMap` (13.8 against 17.8 million operations per second) because of the extra hashing.
- **Sharding changes the hit rate slightly.** Each shard evicts on its own, so the hit rate can differ a little from one cache of the same total size.
