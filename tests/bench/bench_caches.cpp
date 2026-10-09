// Benchmarks JrCache against other header-only C++ caches on the same workloads.
//
//   Put     - insert new keys into a full cache, so every Put evicts one entry
//   GetHit  - look up keys that are all cached
//   GetMiss - look up keys that are not cached
//   Zipf    - a realistic read-through workload: Get, and Put on a miss, with keys drawn
//             from a Zipf distribution (a few keys are very popular). Also reports hit rate.
//   ZipfMT  - Zipf with several threads sharing one cache (thread-safe caches only)
//
// Every cache is wrapped in an adapter with the same two operations, and every benchmark
// replays a pre-generated key sequence, so random number generation is not measured.

#include "cache_map.hpp"
#include "cache_sharded_map.hpp"
#include "utilities.hpp"

#include <benchmark/benchmark.h>

#include <LRUCache11.hpp>
#include <caches/cache.hpp>
#include <caches/fifo_cache_policy.hpp>
#include <caches/lfu_cache_policy.hpp>
#include <caches/lru_cache_policy.hpp>
#include <lrucache.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Key = std::uint64_t;

// ---- Adapters ---------------------------------------------------------------------
// Each adapter has a constructor taking the capacity, `bool Get(key, out)`, which copies
// the value into `out` on a hit, and `void Put(key, value)`.

template <typename V, JrCache::EvictionPolicy Policy, bool ThreadSafe>
class JrCacheAdapter {
    JrCache::CacheMap<Key, V> cache;

public:
    explicit JrCacheAdapter(std::size_t capacity) : cache(capacity, Policy, ThreadSafe) {}
    bool Get(Key key, V& out) {
        if (auto value = cache.Get(key)) {
            out = std::move(*value);
            return true;
        }
        return false;
    }
    void Put(Key key, const V& value) {
        (void)cache.Put(key, value);
    }
};

// Reads through GetPtr: no copy. `out` is only used as a sink so the read is not optimized away.
template <typename V, JrCache::EvictionPolicy Policy>
class JrCachePtrAdapter {
    JrCache::CacheMap<Key, V> cache;

public:
    explicit JrCachePtrAdapter(std::size_t capacity) : cache(capacity, Policy, true) {}
    bool Get(Key key, V&) {
        V* value = cache.GetPtr(key);
        benchmark::DoNotOptimize(value);
        return value != nullptr;
    }
    void Put(Key key, const V& value) {
        (void)cache.Put(key, value);
    }
};

template <typename V, JrCache::EvictionPolicy Policy>
class JrShardedAdapter {
    static constexpr std::size_t shardCount = 16;
    JrCache::ShardedMap<Key, V> cache;

public:
    explicit JrShardedAdapter(std::size_t capacity)
        : cache(shardCount, std::max<std::size_t>(1, capacity / shardCount), Policy) {}
    bool Get(Key key, V& out) {
        if (auto value = cache.Get(key)) {
            out = std::move(*value);
            return true;
        }
        return false;
    }
    void Put(Key key, const V& value) {
        (void)cache.Put(key, value);
    }
};

// vpetrigo/caches: always locks a mutex, stores values in std::shared_ptr.
template <typename V, template <typename> class Policy>
class CachesAdapter {
    caches::fixed_sized_cache<Key, V, Policy> cache;

public:
    explicit CachesAdapter(std::size_t capacity) : cache(capacity) {}
    bool Get(Key key, V& out) {
        auto [value, found] = cache.TryGet(key);
        if (found) out = *value;
        return found;
    }
    void Put(Key key, const V& value) {
        cache.Put(key, value);
    }
};

// lamerman/cpp-lru-cache: no locking. Its get() throws on a miss, so a lookup is
// exists() followed by get(), as its README suggests.
template <typename V>
class LamermanAdapter {
    cache::lru_cache<Key, V> cache;

public:
    explicit LamermanAdapter(std::size_t capacity) : cache(capacity) {}
    bool Get(Key key, V& out) {
        if (!cache.exists(key)) return false;
        out = cache.get(key);
        return true;
    }
    void Put(Key key, const V& value) {
        cache.put(key, value);
    }
};

// mohaps/lrucache11: Lock is lru11::NullLock (no locking) or std::mutex. Elasticity 0 keeps
// it at exactly `capacity` entries, like the other caches.
template <typename V, typename Lock>
class Lru11Adapter {
    lru11::Cache<Key, V, Lock> cache;

public:
    explicit Lru11Adapter(std::size_t capacity) : cache(capacity, 0) {}
    bool Get(Key key, V& out) {
        return cache.tryGet(key, out);
    }
    void Put(Key key, const V& value) {
        cache.insert(key, value);
    }
};

// Not a cache: a std::unordered_map that never evicts, as a lower bound for lookups.
template <typename V>
class UnorderedMapAdapter {
    std::unordered_map<Key, V> map;

public:
    explicit UnorderedMapAdapter(std::size_t capacity) {
        map.reserve(capacity);
    }
    bool Get(Key key, V& out) {
        auto it = map.find(key);
        if (it == map.end()) return false;
        out = it->second;
        return true;
    }
    void Put(Key key, const V& value) {
        map.insert_or_assign(key, value);
    }
};

// ---- Keys and values ------------------------------------------------------------------

// Spreads small integers over the whole 64-bit range (a bijection), so keys are not
// sequential and no hash function gets an easy ride.
Key Scramble(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

template <typename V>
V MakeValue(Key key) {
    if constexpr (std::is_same_v<V, std::string>) {
        // 32 characters: longer than the small-string buffer, so copies allocate.
        return std::string(32, static_cast<char>('a' + key % 26));
    } else {
        return static_cast<V>(key);
    }
}

constexpr std::size_t traceLength = std::size_t{1} << 20; // power of two, for masking
constexpr std::size_t traceMask = traceLength - 1;

// `traceLength` keys drawn uniformly from Scramble(offset) ... Scramble(offset + range - 1).
const std::vector<Key>& UniformTrace(std::size_t range, std::size_t offset) {
    static std::map<std::pair<std::size_t, std::size_t>, std::vector<Key>> traces;
    auto& trace = traces[{range, offset}];
    if (trace.empty()) {
        std::mt19937_64 rng(42);
        std::uniform_int_distribution<std::size_t> pick(0, range - 1);
        trace.reserve(traceLength);
        for (std::size_t i = 0; i < traceLength; ++i) {
            trace.push_back(Scramble(offset + pick(rng)));
        }
    }
    return trace;
}

// `traceLength` keys from `keyspace` distinct keys, where the k-th most popular key is
// requested with probability proportional to 1 / k^skew.
const std::vector<Key>& ZipfTrace(std::size_t keyspace, double skew) {
    static std::map<std::pair<std::size_t, double>, std::vector<Key>> traces;
    auto& trace = traces[{keyspace, skew}];
    if (trace.empty()) {
        std::vector<double> cdf(keyspace);
        double sum = 0;
        for (std::size_t k = 0; k < keyspace; ++k) {
            sum += 1.0 / std::pow(static_cast<double>(k + 1), skew);
            cdf[k] = sum;
        }
        std::mt19937_64 rng(7);
        std::uniform_real_distribution<double> uniform(0.0, sum);
        trace.reserve(traceLength);
        for (std::size_t i = 0; i < traceLength; ++i) {
            const auto rank = std::lower_bound(cdf.begin(), cdf.end(), uniform(rng)) - cdf.begin();
            trace.push_back(Scramble(static_cast<std::uint64_t>(rank)));
        }
    }
    return trace;
}

template <typename V, typename Cache>
void Fill(Cache& cache, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        cache.Put(Scramble(i), MakeValue<V>(i));
    }
}

// One read-through request: Get, and Put the value on a miss. Returns whether it hit.
template <typename V, typename Cache>
bool ReadThrough(Cache& cache, Key key, V& out) {
    if (cache.Get(key, out)) return true;
    out = MakeValue<V>(key);
    cache.Put(key, out);
    return false;
}

// ---- Benchmarks -----------------------------------------------------------------------

template <typename Cache, typename V>
void BM_Put(benchmark::State& state, std::size_t capacity) {
    Cache cache(capacity);
    Fill<V>(cache, capacity);
    const V value = MakeValue<V>(1);
    std::uint64_t next = capacity;
    for (auto _ : state) {
        cache.Put(Scramble(next++), value);
    }
    state.SetItemsProcessed(state.iterations());
}

template <typename Cache, typename V>
void BM_GetHit(benchmark::State& state, std::size_t capacity) {
    Cache cache(capacity);
    Fill<V>(cache, capacity);
    const auto& keys = UniformTrace(capacity, 0);
    V out{};
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.Get(keys[i++ & traceMask], out));
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}

template <typename Cache, typename V>
void BM_GetMiss(benchmark::State& state, std::size_t capacity) {
    Cache cache(capacity);
    Fill<V>(cache, capacity);
    const auto& keys = UniformTrace(capacity, capacity); // none of these were inserted
    V out{};
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(cache.Get(keys[i++ & traceMask], out));
    }
    state.SetItemsProcessed(state.iterations());
}

constexpr std::size_t zipfCapacity = 10'000;
constexpr std::size_t zipfKeyspace = 100'000;
constexpr double zipfSkew = 0.99;

template <typename Cache, typename V>
void BM_Zipf(benchmark::State& state) {
    Cache cache(zipfCapacity);
    const auto& keys = ZipfTrace(zipfKeyspace, zipfSkew);
    V out{};
    for (std::size_t i = 0; i < traceLength; ++i) { // warm up so the cache starts full
        ReadThrough(cache, keys[i], out);
    }
    std::size_t i = 0;
    std::int64_t hits = 0;
    for (auto _ : state) {
        hits += ReadThrough(cache, keys[i++ & traceMask], out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["hit%"] = 100.0 * static_cast<double>(hits) / static_cast<double>(state.iterations());
}

// One cache shared by all threads of a ZipfMT run, created in Setup before the threads start.
template <typename Cache>
struct SharedCache {
    static inline std::unique_ptr<Cache> cache;
};

template <typename Cache, typename V>
void SetupShared(const benchmark::State&) {
    SharedCache<Cache>::cache = std::make_unique<Cache>(zipfCapacity);
    const auto& keys = ZipfTrace(zipfKeyspace, zipfSkew);
    V out{};
    for (std::size_t i = 0; i < traceLength; ++i) {
        ReadThrough(*SharedCache<Cache>::cache, keys[i], out);
    }
}

template <typename Cache>
void TeardownShared(const benchmark::State&) {
    SharedCache<Cache>::cache.reset();
}

template <typename Cache, typename V>
void BM_ZipfMT(benchmark::State& state) {
    Cache& cache = *SharedCache<Cache>::cache;
    const auto& keys = ZipfTrace(zipfKeyspace, zipfSkew);
    // Each thread starts at a different point of the trace.
    std::size_t i = static_cast<std::size_t>(state.thread_index()) * (traceLength / 64);
    V out{};
    std::int64_t hits = 0;
    for (auto _ : state) {
        hits += ReadThrough(cache, keys[i++ & traceMask], out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["hit%"] = benchmark::Counter(
        100.0 * static_cast<double>(hits) / static_cast<double>(state.iterations()),
        benchmark::Counter::kAvgThreads);
}

// ---- Registration -----------------------------------------------------------------------

using U64 = std::uint64_t;
using JrPolicy = JrCache::EvictionPolicy;

template <typename V>
struct SingleThreaded {
    template <typename Cache>
    static void Add(const std::string& library, std::size_t capacity, bool evicts = true) {
        if (evicts) {
            benchmark::RegisterBenchmark("Put/" + std::to_string(capacity) + "/" + library,
                                         BM_Put<Cache, V>, capacity);
        }
        benchmark::RegisterBenchmark("GetHit/" + std::to_string(capacity) + "/" + library,
                                     BM_GetHit<Cache, V>, capacity);
        benchmark::RegisterBenchmark("GetMiss/" + std::to_string(capacity) + "/" + library,
                                     BM_GetMiss<Cache, V>, capacity);
    }
};

void RegisterAll() {
    for (std::size_t capacity : {std::size_t{1'000}, std::size_t{100'000}}) {
        using S = SingleThreaded<U64>;
        S::Add<JrCacheAdapter<U64, JrPolicy::LRU, true>>("JrCache.LRU", capacity);
        S::Add<JrCacheAdapter<U64, JrPolicy::LRU, false>>("JrCache.LRU.unlocked", capacity);
        S::Add<CachesAdapter<U64, caches::LRUCachePolicy>>("caches.LRU", capacity);
        S::Add<LamermanAdapter<U64>>("cpp-lru-cache", capacity);
        S::Add<Lru11Adapter<U64, lru11::NullLock>>("lrucache11", capacity);
        S::Add<JrCacheAdapter<U64, JrPolicy::LFU, true>>("JrCache.LFU", capacity);
        S::Add<CachesAdapter<U64, caches::LFUCachePolicy>>("caches.LFU", capacity);
        S::Add<JrCacheAdapter<U64, JrPolicy::FIFO, true>>("JrCache.FIFO", capacity);
        S::Add<CachesAdapter<U64, caches::FIFOCachePolicy>>("caches.FIFO", capacity);
        S::Add<UnorderedMapAdapter<U64>>("unordered_map(no-eviction)", capacity, false);
    }

    // Values that allocate when copied: shows the cost of returning copies.
    {
        constexpr std::size_t capacity = 10'000;
        using S = std::string;
        auto add = [&](const std::string& library, auto bm) {
            benchmark::RegisterBenchmark("GetHit.string/" + std::to_string(capacity) + "/" + library,
                                         bm, capacity);
        };
        add("JrCache.LRU", BM_GetHit<JrCacheAdapter<S, JrPolicy::LRU, true>, S>);
        add("JrCache.LRU.GetPtr", BM_GetHit<JrCachePtrAdapter<S, JrPolicy::LRU>, S>);
        add("caches.LRU", BM_GetHit<CachesAdapter<S, caches::LRUCachePolicy>, S>);
        add("cpp-lru-cache", BM_GetHit<LamermanAdapter<S>, S>);
        add("lrucache11", BM_GetHit<Lru11Adapter<S, lru11::NullLock>, S>);
    }

    // Zipf read-through: speed and hit rate of each policy.
    {
        auto add = [](const std::string& library, auto bm) {
            benchmark::RegisterBenchmark("Zipf/" + library, bm);
        };
        add("JrCache.LRU", BM_Zipf<JrCacheAdapter<U64, JrPolicy::LRU, true>, U64>);
        add("JrCache.LRU.unlocked", BM_Zipf<JrCacheAdapter<U64, JrPolicy::LRU, false>, U64>);
        add("caches.LRU", BM_Zipf<CachesAdapter<U64, caches::LRUCachePolicy>, U64>);
        add("cpp-lru-cache", BM_Zipf<LamermanAdapter<U64>, U64>);
        add("lrucache11", BM_Zipf<Lru11Adapter<U64, lru11::NullLock>, U64>);
        add("JrCache.LFU", BM_Zipf<JrCacheAdapter<U64, JrPolicy::LFU, true>, U64>);
        add("caches.LFU", BM_Zipf<CachesAdapter<U64, caches::LFUCachePolicy>, U64>);
        add("JrCache.FIFO", BM_Zipf<JrCacheAdapter<U64, JrPolicy::FIFO, true>, U64>);
        add("caches.FIFO", BM_Zipf<CachesAdapter<U64, caches::FIFOCachePolicy>, U64>);
    }

    // Zipf read-through with threads sharing one cache.
    {
        const int maxThreads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        auto add = [&]<typename Cache>(const std::string& library) {
            auto* bm = benchmark::RegisterBenchmark("ZipfMT/" + library, BM_ZipfMT<Cache, U64>);
            bm->Setup(SetupShared<Cache, U64>)->Teardown(TeardownShared<Cache>)->UseRealTime();
            for (int threads = 1; threads <= maxThreads; threads *= 2) {
                bm->Threads(threads);
            }
        };
        add.operator()<JrCacheAdapter<U64, JrPolicy::LRU, true>>("JrCache.LRU");
        add.operator()<JrShardedAdapter<U64, JrPolicy::LRU>>("JrCache.ShardedLRU(16)");
        add.operator()<CachesAdapter<U64, caches::LRUCachePolicy>>("caches.LRU");
        add.operator()<Lru11Adapter<U64, std::mutex>>("lrucache11(mutex)");
    }
}

} // namespace

int main(int argc, char** argv) {
    RegisterAll();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
