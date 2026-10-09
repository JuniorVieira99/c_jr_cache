#pragma once

#include "cache_buckets.hpp"
#include "cache_queue.hpp"
#include "cache_serialization.hpp"
#include "utilities.hpp"

#include <concepts>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace JrCache {

template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible_v<KeyType> && std::is_copy_constructible_v<ValueType> &&
             (sizeof(KeyType) > 0)
class ShardedMap;

// A key-value cache that holds at most `Capacity()` entries. When a new key is added to a
// full cache, one entry is evicted first, chosen by the eviction policy:
//   LRU  - least recently used by Get or Put.     
//   MRU  - most recently used by Get or Put.
//   FIFO - inserted longest ago.              
//   LIFO - inserted most recently.
//   LFU  - fewest Get and Put calls.              
//   MFU  - most Get and Put calls.
//
// Ties between LFU or MFU candidates are broken arbitrarily.
template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible_v<KeyType> && std::is_copy_constructible_v<ValueType> &&
             (sizeof(KeyType) > 0)
class CacheMap {

    using Item = std::pair<KeyType, ValueType>;

    // ShardedMap uses the snapshot functions below to load all of its shards together.
    template <typename K, typename V>
        requires std::is_copy_constructible_v<K> && std::is_copy_constructible_v<V> &&
                 (sizeof(K) > 0)
    friend class ShardedMap;

    JrCache::EvictionPolicy evictionPolicy;

    // LRU, MRU, FIFO and LIFO: `entries` owns the values and `cacheQueue` orders pointers to
    // them, newest or most recently used at the top. unordered_map never moves its elements,
    // so those pointers stay valid until the entry is erased.
    std::unordered_map<KeyType, ValueType> entries;
    JrCache::LinkedList<KeyType, ValueType> cacheQueue{false};

    // LFU and MFU: `cacheBuckets` owns the values, in buckets indexed by use count, and
    // `useCounts` records which bucket each key is in.
    std::unordered_map<KeyType, std::size_t> useCounts;
    JrCache::BucketMap<KeyType, ValueType> cacheBuckets{false};

    std::size_t maxSize;
    const bool locked;
    mutable std::mutex mtx;

    // The inner containers are created without locking; this lock guards every public call.
    [[nodiscard]]
    std::unique_lock<std::mutex> acquire() const {
        if (!locked) return {};
        return std::unique_lock<std::mutex>(mtx);
    }

    static bool countsUses(JrCache::EvictionPolicy policy) {
        return policy == JrCache::EvictionPolicy::LFU || policy == JrCache::EvictionPolicy::MFU;
    }

    bool countsUses() const {
        return countsUses(evictionPolicy);
    }

    bool reordersOnUse() const {
        return evictionPolicy == JrCache::EvictionPolicy::LRU ||
               evictionPolicy == JrCache::EvictionPolicy::MRU;
    }

    std::size_t size() const {
        return countsUses() ? useCounts.size() : entries.size();
    }

    // Removes `key` from the queue and from `entries` and returns it. Caller holds the lock.
    std::optional<Item> takeQueued(const std::optional<KeyType>& key) {
        if (!key) return std::nullopt;
        auto item = entries.extract(*key);
        if (item.empty()) return std::nullopt;
        cacheQueue.Remove(*key);
        return std::make_pair(std::move(item.key()), std::move(item.mapped()));
    }

    // Removes and returns the entry the eviction policy picks. Caller holds the lock.
    std::optional<Item> evict() {
        switch (evictionPolicy) {
        case JrCache::EvictionPolicy::LFU:
        case JrCache::EvictionPolicy::MFU: {
            auto index = evictionPolicy == JrCache::EvictionPolicy::LFU
                             ? cacheBuckets.LowestIndex()
                             : cacheBuckets.HighestIndex();
            if (!index) return std::nullopt;
            auto removed = cacheBuckets.RemoveAny(*index);
            if (removed) {
                useCounts.erase(removed->first);
            }
            return removed;
        }
        case JrCache::EvictionPolicy::MRU:
        case JrCache::EvictionPolicy::LIFO:
            return takeQueued(cacheQueue.GetTopKey());
        default:
            return takeQueued(cacheQueue.GetBottomKey());
        }
    }

    // Records a use of `key` and returns a pointer to its stored value, or nullptr if it is
    // not cached. The value never moves while it is cached: LFU and MFU move the map node
    // between buckets, not the value. Caller holds the lock.
    ValueType* find(const KeyType& key) {
        if (countsUses()) {
            auto it = useCounts.find(key);
            if (it == useCounts.end()) return nullptr;
            ValueType* value = cacheBuckets.MoveValue(key, it->second, it->second + 1);
            if (value) ++it->second;
            return value;
        }
        auto it = entries.find(key);
        if (it == entries.end()) return nullptr;
        if (reordersOnUse()) {
            cacheQueue.SetTop(key, &it->second);
        }
        return &it->second;
    }

    std::optional<Item> put(const KeyType& key, const ValueType& value) {
        if (countsUses()) {
            auto it = useCounts.find(key);
            if (it != useCounts.end()) {
                ValueType* stored = cacheBuckets.MoveValue(key, it->second, it->second + 1);
                ++it->second;
                *stored = value;
                return std::nullopt;
            }
            std::optional<Item> evicted;
            if (size() >= maxSize) evicted = evict();
            cacheBuckets.PutValue(key, value, 1);
            try {
                useCounts.emplace(key, 1);
            } catch (...) {
                cacheBuckets.RemoveValue(key, 1);
                throw;
            }
            return evicted;
        }

        auto it = entries.find(key);
        if (it != entries.end()) {
            it->second = value;
            if (reordersOnUse()) {
                cacheQueue.SetTop(key, &it->second);
            }
            return std::nullopt;
        }
        std::optional<Item> evicted;
        if (size() >= maxSize) evicted = evict();
        it = entries.emplace(key, value).first;
        try {
            cacheQueue.SetTop(key, &it->second);
        } catch (...) {
            entries.erase(it);
            throw;
        }
        return evicted;
    }

    std::optional<Item> remove(const KeyType& key) {
        if (countsUses()) {
            auto it = useCounts.find(key);
            if (it == useCounts.end()) return std::nullopt;
            auto removed = cacheBuckets.RemoveValue(key, it->second);
            useCounts.erase(it);
            return removed;
        }
        return takeQueued(key);
    }

    // The contents of a cache as read from JSON, before they are loaded.
    struct Snapshot {
        // LRU, MRU, FIFO and LIFO: entries from the top of the queue to the bottom.
        std::vector<Item> entries;
        // LFU and MFU: (use count, entries) pairs. A use count may appear more than once.
        std::vector<std::pair<std::size_t, std::vector<Item>>> buckets;
    };

    // Returns the cache as JSON. Caller holds the lock.
    nlohmann::json toJson() const
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        nlohmann::json out = nlohmann::json::object();
        out["policy"] = evictionPolicy;
        out["capacity"] = maxSize;
        if (countsUses()) {
            out["buckets"] = cacheBuckets.ToJson();
        } else {
            out["entries"] = cacheQueue.ToJson();
        }
        return out;
    }

    // Reads a cache returned by ToJson. Throws std::invalid_argument if it was written by a
    // cache with a different policy than `policy`.
    static Snapshot readSnapshot(const nlohmann::json& json, JrCache::EvictionPolicy policy)
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        const auto savedPolicy = json.at("policy").get<JrCache::EvictionPolicy>();
        if (savedPolicy != policy) {
            throw std::invalid_argument("the JSON was written by a " +
                                        std::string(PolicyName(savedPolicy)) +
                                        " cache, but this cache uses " +
                                        std::string(PolicyName(policy)));
        }
        Snapshot snapshot;
        if (countsUses(policy)) {
            snapshot.buckets = JrCache::BucketMap<KeyType, ValueType>::ReadJson(json.at("buckets"));
        } else {
            snapshot.entries = JrCache::LinkedList<KeyType, ValueType>::ReadJson(json.at("entries"));
        }
        return snapshot;
    }

    // Throws std::invalid_argument if `snapshot` does not fit this cache.
    void checkSnapshot(const Snapshot& snapshot) const {
        std::unordered_set<KeyType> keys;
        auto addKey = [&](const KeyType& key) {
            if (!keys.insert(key).second) {
                throw std::invalid_argument("the JSON contains the same key more than once");
            }
        };
        for (const auto& item : snapshot.entries) {
            addKey(item.first);
        }
        for (const auto& bucket : snapshot.buckets) {
            for (const auto& item : bucket.second) {
                addKey(item.first);
            }
        }
        if (keys.size() > maxSize) {
            throw std::invalid_argument("the JSON has " + std::to_string(keys.size()) +
                                        " entries, but the cache capacity is " +
                                        std::to_string(maxSize));
        }
    }

    // Replaces the contents with `snapshot`, which must have passed checkSnapshot.
    void restore(Snapshot&& snapshot) {
        auto lock = acquire();
        cacheQueue.Clear();
        entries.clear();
        cacheBuckets.Clear();
        useCounts.clear();
        for (auto& [key, value] : snapshot.entries) {
            auto it = entries.emplace(key, std::move(value)).first;
            cacheQueue.SetBottom(key, &it->second);
        }
        for (const auto& [uses, items] : snapshot.buckets) {
            for (const auto& item : items) {
                useCounts.emplace(item.first, uses);
            }
            cacheBuckets.PutBatch(items, uses);
        }
    }

public:
    // Throws std::invalid_argument if `capacity` is 0.
    explicit CacheMap(std::size_t capacity,
                      JrCache::EvictionPolicy policy = JrCache::EvictionPolicy::LRU,
                      bool threadSafe = true)
        : evictionPolicy(policy), maxSize(capacity), locked(threadSafe) {
        if (capacity == 0) {
            throw std::invalid_argument("CacheMap capacity must be at least 1");
        }
    }
    CacheMap(const CacheMap&) = delete;
    CacheMap& operator=(const CacheMap&) = delete;

    // There are three ways to read a value. All of them count as a use for LRU, MRU, LFU
    // and MFU:
    //   Get     returns a copy. Always safe, but copying a large value costs time.
    //   GetPtr  returns a pointer to the cached value itself. No copy, but see its warning.
    //   Visit   calls a function with the cached value itself while the cache is locked.
    //           No copy, and safe when other threads use the cache.

    // Returns a copy of the value for `key`, or nullopt if it is not cached.
    [[nodiscard]]
    std::optional<ValueType> Get(const KeyType& key) {
        auto lock = acquire();
        if (const ValueType* value = find(key)) {
            return *value;
        }
        return std::nullopt;
    }

    // Returns a pointer to the cached value for `key`, or nullptr if it is not cached. The
    // value can be read or changed through the pointer without copying it.
    //
    // The pointer stays valid until the entry leaves the cache: through Remove, Clear,
    // FromJson, or eviction when a Put adds another key. Putting a new value for the same key
    // updates the value in place, so the pointer stays valid. The cache's lock is NOT held
    // while you use the pointer, so if other threads may Put, Remove or Clear, use Visit or
    // Get instead.
    [[nodiscard]]
    ValueType* GetPtr(const KeyType& key) {
        auto lock = acquire();
        return find(key);
    }

    // If `key` is cached, calls `visitor(value)` with the cached value itself, while holding
    // the cache's lock, and returns true. Returns false if it is not cached. The visitor may
    // read or change the value, but must not call this cache, and should be quick, since
    // other threads wait for it.
    template <typename Visitor>
        requires std::invocable<Visitor&, ValueType&>
    bool Visit(const KeyType& key, Visitor&& visitor) {
        auto lock = acquire();
        ValueType* value = find(key);
        if (!value) return false;
        visitor(*value);
        return true;
    }

    // Inserts `key` or replaces its value. Counts as a use for LRU, MRU, LFU and MFU; FIFO
    // and LIFO keep a replaced key in its original insertion position.
    //
    // Returns the entry that was EVICTED to make room for `key`, not the one just inserted.
    // It is nullopt when nothing was evicted: the cache had space, or `key` was already cached.
    // Example (LRU, capacity 2): Put(1, "a"); Put(2, "b"); Put(3, "c") returns {1, "a"}.
    std::optional<std::pair<KeyType, ValueType>> Put(const KeyType& key, const ValueType& value) {
        auto lock = acquire();
        return put(key, value);
    }
    
    // Removes `key` and returns its key-value pair, or nullopt if it was not cached.
    std::optional<std::pair<KeyType, ValueType>> Remove(const KeyType& key) {
        auto lock = acquire();
        return remove(key);
    }

    // Does not count as a use.
    [[nodiscard]]
    bool Contains(const KeyType& key) const {
        auto lock = acquire();
        return countsUses() ? useCounts.contains(key) : entries.contains(key);
    }

    void Clear() {
        auto lock = acquire();
        cacheQueue.Clear();
        entries.clear();
        cacheBuckets.Clear();
        useCounts.clear();
    }

    [[nodiscard]]
    std::size_t Size() const {
        auto lock = acquire();
        return size();
    }

    [[nodiscard]]
    bool IsEmpty() const {
        auto lock = acquire();
        return size() == 0;
    }

    [[nodiscard]]
    std::size_t Capacity() const {
        return maxSize;
    }

    [[nodiscard]]
    JrCache::EvictionPolicy GetEvictionPolicy() const {
        return evictionPolicy;
    }

    // Serialization

    // Returns the whole cache as JSON, including the order or use counts that decide what is
    // evicted next. Call dump() on the result for a string.
    //   LRU, MRU, FIFO, LIFO: {"policy":"LRU","capacity":N,"entries":[...]}, top of the queue first
    //   LFU, MFU:             {"policy":"LFU","capacity":N,"buckets":[...]}, grouped by use count
    [[nodiscard]]
    nlohmann::json ToJson() const
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        auto lock = acquire();
        return toJson();
    }

    // Replaces the contents with JSON returned by ToJson, restoring the eviction order or use
    // counts too. To load a string, pass nlohmann::json::parse(text). Throws
    // nlohmann::json::exception if `json` does not have the expected shape, and
    // std::invalid_argument if it was written with another eviction policy, repeats a key or
    // has more entries than this cache's capacity. If it throws, the cache is unchanged.
    void FromJson(const nlohmann::json& json)
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        Snapshot snapshot = readSnapshot(json, evictionPolicy);
        checkSnapshot(snapshot);
        restore(std::move(snapshot));
    }
};

} // namespace JrCache
