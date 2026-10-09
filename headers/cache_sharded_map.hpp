#pragma once

#include "cache_map.hpp"
#include "cache_serialization.hpp"
#include "utilities.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace JrCache {

template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible_v<KeyType> && std::is_copy_constructible_v<ValueType> &&
             (sizeof(KeyType) > 0)
class Shard {

    std::size_t maxSizePerShard;
    JrCache::CacheMap<KeyType, ValueType> cacheMap;

public:

    Shard(std::size_t maxSizePerShard, JrCache::EvictionPolicy evictionPolicy)
        : maxSizePerShard(maxSizePerShard), cacheMap(maxSizePerShard, evictionPolicy, true) {
    }

    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    JrCache::CacheMap<KeyType, ValueType>& GetCacheMap() {
        return this->cacheMap;
    }

    const JrCache::CacheMap<KeyType, ValueType>& GetCacheMap() const {
        return this->cacheMap;
    }
};

template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible_v<KeyType> && std::is_copy_constructible_v<ValueType> &&
             (sizeof(KeyType) > 0)
class ShardedMap {

    std::size_t shardCount;
    std::size_t maxSizePerShard;
    JrCache::EvictionPolicy evictionPolicy;
    std::vector<std::unique_ptr<Shard<KeyType, ValueType>>> shards;

    // Picks the shard for `key`. The hash is mixed first because std::hash<int> and similar
    // are the identity on GCC and Clang, which would send keys 0, 8, 16, ... all to shard 0
    // of an 8-shard map.
    std::size_t getShardIndex(const KeyType& key) const {
        std::uint64_t h = static_cast<std::uint64_t>(std::hash<KeyType>{}(key));
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return static_cast<std::size_t>(h % shardCount);
    }

    Shard<KeyType, ValueType>& getShard(const KeyType& key) {
        return *shards[getShardIndex(key)];
    }

    const Shard<KeyType, ValueType>& getShard(const KeyType& key) const {
        return *shards[getShardIndex(key)];
    }

public:
    // Splits `shardCount * maxSizePerShard` entries of capacity across independent shards,
    // each with its own lock, so threads using keys in different shards do not block each
    // other. Eviction happens per shard: a full shard evicts from itself even if others have
    // space. Throws std::invalid_argument if `shardCount` or `maxSizePerShard` is 0.
    ShardedMap(std::size_t shardCount, std::size_t maxSizePerShard, JrCache::EvictionPolicy evictionPolicy)
        : shardCount(shardCount), maxSizePerShard(maxSizePerShard), evictionPolicy(evictionPolicy), shards(shardCount) {
        if (shardCount == 0) {
            throw std::invalid_argument("ShardedMap needs at least 1 shard");
        }
        for (std::size_t i = 0; i < shardCount; ++i) {
            shards[i] = std::make_unique<Shard<KeyType, ValueType>>(maxSizePerShard, evictionPolicy);
        }
    }

    ShardedMap(const ShardedMap&) = delete;
    ShardedMap& operator=(const ShardedMap&) = delete;

    // Returns the entry evicted from this key's shard to make room, if any.
    std::optional<std::pair<KeyType, ValueType>> Put(const KeyType& key, const ValueType& value) {
        Shard<KeyType, ValueType>& shard = getShard(key);
        return shard.GetCacheMap().Put(key, value);
    }

    // Returns a copy of the value for `key`, or nullopt if it is not cached.
    [[nodiscard]]
    std::optional<ValueType> Get(const KeyType& key) {
        Shard<KeyType, ValueType>& shard = getShard(key);
        return shard.GetCacheMap().Get(key);
    }

    // Returns a pointer to the cached value itself, or nullptr. Same rules as
    // CacheMap::GetPtr: the shard's lock is not held while you use the pointer, so prefer
    // Visit or Get when other threads may change the map.
    [[nodiscard]]
    ValueType* GetPtr(const KeyType& key) {
        return getShard(key).GetCacheMap().GetPtr(key);
    }

    // Calls `visitor(value)` with the cached value itself while holding its shard's lock.
    // Returns false if `key` is not cached. Same rules as CacheMap::Visit.
    template <typename Visitor>
        requires std::invocable<Visitor&, ValueType&>
    bool Visit(const KeyType& key, Visitor&& visitor) {
        return getShard(key).GetCacheMap().Visit(key, std::forward<Visitor>(visitor));
    }

    // Removes `key` and returns its key-value pair, or nullopt if it was not cached.
    std::optional<std::pair<KeyType, ValueType>> Remove(const KeyType& key) {
        Shard<KeyType, ValueType>& shard = getShard(key);
        return shard.GetCacheMap().Remove(key);
    }

    // Does not count as a use.
    [[nodiscard]]
    bool Contains(const KeyType& key) const {
        const Shard<KeyType, ValueType>& shard = getShard(key);
        return shard.GetCacheMap().Contains(key);
    }

    void Clear() {
        for (std::unique_ptr<Shard<KeyType, ValueType>>& shard : shards) {
            shard->GetCacheMap().Clear();
        }
    }

    // Shards are locked one at a time, so while other threads are writing this is an
    // approximation rather than a snapshot.
    [[nodiscard]]
    std::size_t Size() const {
        std::size_t totalSize = 0;
        for (const std::unique_ptr<Shard<KeyType, ValueType>>& shard : shards) {
            totalSize += shard->GetCacheMap().Size();
        }
        return totalSize;
    }

    [[nodiscard]]
    bool IsEmpty() const {
        for (const std::unique_ptr<Shard<KeyType, ValueType>>& shard : shards) {
            if (!shard->GetCacheMap().IsEmpty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    std::size_t Capacity() const {
        std::size_t totalCapacity = 0;
        for (const std::unique_ptr<Shard<KeyType, ValueType>>& shard : shards) {
            totalCapacity += shard->GetCacheMap().Capacity();
        }
        return totalCapacity;
    }

    [[nodiscard]]
    JrCache::EvictionPolicy GetEvictionPolicy() const {
        return evictionPolicy;
    }

    // Serialization

    // Returns every shard as JSON: {"policy":"LRU","shards":[<CacheMap JSON>,...]}.
    // Shards are read one at a time, so while other threads are writing, the result is not a
    // single point-in-time snapshot of the whole map.
    [[nodiscard]]
    nlohmann::json ToJson() const
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        nlohmann::json out = nlohmann::json::object();
        out["policy"] = evictionPolicy;
        out["shards"] = nlohmann::json::array();
        for (const auto& shard : shards) {
            out["shards"].push_back(shard->GetCacheMap().ToJson());
        }
        return out;
    }

    // Replaces the contents with JSON returned by ToJson. Every key is placed in the shard it
    // belongs to in this map, so the JSON may come from a map with a different shard count or
    // a different std::hash. Keys that end up in the same shard keep their relative order (LRU,
    // MRU, FIFO, LIFO) or use counts (LFU, MFU).
    //
    // Throws nlohmann::json::exception if `json` does not have the expected shape, and
    // std::invalid_argument if it was written with another eviction policy, repeats a key or
    // would put more entries in a shard than it can hold. If it throws, the map is unchanged.
    // Shards are replaced one at a time, so other threads may briefly see some shards loaded
    // and others not.
    void FromJson(const nlohmann::json& json)
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        using Cache = JrCache::CacheMap<KeyType, ValueType>;

        const auto savedPolicy = json.at("policy").get<JrCache::EvictionPolicy>();
        if (savedPolicy != evictionPolicy) {
            throw std::invalid_argument("the JSON was written by a " +
                                        std::string(PolicyName(savedPolicy)) +
                                        " map, but this map uses " +
                                        std::string(PolicyName(evictionPolicy)));
        }

        std::vector<typename Cache::Snapshot> routed(shardCount);
        for (const auto& shard : detail::asArray(json.at("shards"))) {
            route(Cache::readSnapshot(shard, evictionPolicy), routed);
        }

        for (std::size_t i = 0; i < shardCount; ++i) {
            shards[i]->GetCacheMap().checkSnapshot(routed[i]);
        }
        for (std::size_t i = 0; i < shardCount; ++i) {
            shards[i]->GetCacheMap().restore(std::move(routed[i]));
        }
    }

private:
    // Moves every entry of `source` into the snapshot of the shard its key belongs to.
    void route(typename JrCache::CacheMap<KeyType, ValueType>::Snapshot&& source,
               std::vector<typename JrCache::CacheMap<KeyType, ValueType>::Snapshot>& routed) const {
        for (auto& item : source.entries) {
            routed[getShardIndex(item.first)].entries.push_back(std::move(item));
        }
        for (auto& [uses, items] : source.buckets) {
            for (auto& item : items) {
                auto& target = routed[getShardIndex(item.first)].buckets;
                if (target.empty() || target.back().first != uses) {
                    target.emplace_back(uses, std::vector<std::pair<KeyType, ValueType>>{});
                }
                target.back().second.push_back(std::move(item));
            }
        }
    }
};

} // namespace JrCache
