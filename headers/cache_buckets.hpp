#pragma once

#include "cache_serialization.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <type_traits>
#include <mutex>
#include <map>
#include <vector>

namespace JrCache {

template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible<KeyType>::value &&
             std::is_copy_constructible<ValueType>::value && (sizeof(KeyType) > 0)
class bucket{
    std::unordered_map<KeyType, ValueType> storage;

public:
    using node_type = typename std::unordered_map<KeyType, ValueType>::node_type;

    bucket() = default;
    bucket(const bucket&) = delete;
    bucket& operator=(const bucket&) = delete;


    [[nodiscard]]
    std::optional<std::pair<KeyType, ValueType>> Get(const KeyType& key) const {
        auto it = storage.find(key);
        if (it != storage.end()) {
            return std::make_optional(std::make_pair(it->first, it->second));
        }
        return std::nullopt;
    }

    [[nodiscard]] 
    std::optional<std::pair<KeyType, ValueType>> GetAny() const {
        return storage.empty() ? std::nullopt : std::make_optional(*storage.begin());
    }

    void Put(const KeyType& key, const ValueType& value) {
        storage[key] = value;
    }

    // Returns a pointer to the stored value, or nullptr if `key` is not here.
    [[nodiscard]]
    ValueType* Find(const KeyType& key) {
        auto it = storage.find(key);
        return it == storage.end() ? nullptr : &it->second;
    }

    // Takes `key` out of the bucket without copying or destroying its value. The returned
    // node is empty if `key` was not here.
    node_type Extract(const KeyType& key) {
        return storage.extract(key);
    }

    // Puts a node taken out by Extract into this bucket and returns a pointer to its value,
    // which keeps the address it had before. If the key is already here, its value is
    // replaced instead. If this throws, `node` still holds the item.
    ValueType* Insert(node_type&& node) {
        auto result = storage.insert(std::move(node));
        if (!result.inserted) {
            result.position->second = std::move(result.node.mapped());
        }
        return &result.position->second;
    }

    // Removes `key` and returns its key-value pair, or nullopt if it was not here.
    std::optional<std::pair<KeyType, ValueType>> Remove(const KeyType& key) {
        auto item = storage.extract(key);
        if (item.empty()) {
            return std::nullopt;
        }
        return std::make_pair(std::move(item.key()), std::move(item.mapped()));
    }

    // Removes an arbitrary item and returns it, or nullopt if the bucket is empty.
    std::optional<std::pair<KeyType, ValueType>> RemoveAny() {
        if (storage.empty()) {
            return std::nullopt;
        }
        auto item = storage.extract(storage.begin());
        return std::make_pair(std::move(item.key()), std::move(item.mapped()));
    }

    void Clear() {
        storage.clear();
    }

    [[nodiscard]]
    bool Contains(const KeyType& key) const {
        return storage.find(key) != storage.end();
    }

    [[nodiscard]]
    std::optional<std::vector<std::pair<KeyType, ValueType>>> GetAll() const {
        if (storage.empty()) {
            return std::nullopt;
        }
        std::vector<std::pair<KeyType, ValueType>> all_items;
        all_items.reserve(storage.size());
        for (const auto& item : storage) {
            all_items.emplace_back(item.first, item.second);
        }
        return std::make_optional(std::move(all_items));
    }

    void PutBulk(const std::vector<std::pair<KeyType, ValueType>>& items) {
        for (const auto& item : items) {
            storage[item.first] = item.second;
        }
    }

    // Removes every key that is present and returns the removed pairs in the order of `keys`.
    std::vector<std::pair<KeyType, ValueType>> RemoveBulk(const std::vector<KeyType>& keys) {
        std::vector<std::pair<KeyType, ValueType>> removed;
        for (const auto& key : keys) {
            if (auto item = Remove(key)) {
                removed.push_back(std::move(*item));
            }
        }
        return removed;
    }

    // Empties the bucket and returns everything it held.
    std::vector<std::pair<KeyType, ValueType>> RemoveAll() {
        std::vector<std::pair<KeyType, ValueType>> removed;
        removed.reserve(storage.size());
        while (auto item = RemoveAny()) {
            removed.push_back(std::move(*item));
        }
        return removed;
    }

    [[nodiscard]]
    std::size_t Size() const {
        return storage.size();
    }

    [[nodiscard]]
    bool IsEmpty() const {
        return storage.empty();
    }

    // Returns the items as [{"key":K,"value":V},...], in no particular order.
    [[nodiscard]]
    nlohmann::json ToJson() const
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& [key, value] : storage) {
            out.push_back(detail::keyValueToJson(key, value));
        }
        return out;
    }
};


template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible<KeyType>::value &&
             std::is_copy_constructible<ValueType>::value && (sizeof(KeyType) > 0)
class BucketMap{

    std::map<std::size_t, bucket<KeyType, ValueType>> storage;

    const bool Locked;
    mutable std::mutex mtx;

    [[nodiscard]]
    std::unique_lock<std::mutex> acquire() const {
        if (!Locked) return {};
        return std::unique_lock<std::mutex>(mtx);
    }

    // Drops the bucket at `it` once it holds no items, so Size() only counts
    // buckets that hold something. Caller holds the lock.
    void eraseIfEmpty(typename std::map<std::size_t, bucket<KeyType, ValueType>>::iterator it) {
        if (it->second.IsEmpty()) {
            storage.erase(it);
        }
    }

public:
    explicit BucketMap(bool locked = false) : Locked(locked) {}
    BucketMap(const BucketMap&) = delete;
    BucketMap& operator=(const BucketMap&) = delete;

    [[nodiscard]]
    std::optional<ValueType> GetValue(const KeyType& key, std::size_t index) const {
        auto lock = acquire();
        auto it = storage.find(index);
        if (it != storage.end()) {
            auto value_opt = it->second.Get(key);
            if (value_opt.has_value()) {
                return std::make_optional(value_opt->second);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    std::optional<ValueType> GetAny(std::size_t index) const {
        auto lock = acquire();
        auto it = storage.find(index);
        if (it != storage.end()) {
            if (it->second.Size() > 0) {
                return it->second.GetAny()->second;
            }
        }
        return std::nullopt;
    }

    void PutValue(const KeyType& key, const ValueType& value, std::size_t index) {
        auto lock = acquire();
        storage[index].Put(key, value);
    }

    void PutBatch(const std::vector<std::pair<KeyType, ValueType>>& items, std::size_t index) {
        if (items.empty()) return;
        auto lock = acquire();
        storage[index].PutBulk(items);
    }

    // Moves `key` from bucket `from` to bucket `to` without copying its value, and returns a
    // pointer to the value, which keeps its address. Returns nullptr, and changes nothing,
    // if `key` is not in bucket `from`.
    ValueType* MoveValue(const KeyType& key, std::size_t from, std::size_t to) {
        auto lock = acquire();
        auto source = storage.find(from);
        if (source == storage.end()) {
            return nullptr;
        }
        if (from == to) {
            return source->second.Find(key);
        }
        auto target = storage.try_emplace(to).first; // before anything moves, in case it throws
        auto node = source->second.Extract(key);
        if (node.empty()) {
            eraseIfEmpty(target);
            return nullptr;
        }
        ValueType* value = nullptr;
        try {
            value = target->second.Insert(std::move(node));
        } catch (...) {
            source->second.Insert(std::move(node));
            eraseIfEmpty(target);
            throw;
        }
        eraseIfEmpty(source);
        return value;
    }

    // Removes `key` from bucket `index` and returns its key-value pair, or nullopt if absent.
    std::optional<std::pair<KeyType, ValueType>> RemoveValue(const KeyType& key,
                                                             std::size_t index) {
        auto lock = acquire();
        auto it = storage.find(index);
        if (it == storage.end()) {
            return std::nullopt;
        }
        auto removed = it->second.Remove(key);
        eraseIfEmpty(it);
        return removed;
    }

    // Removes the given keys from bucket `index` and returns the pairs that were present,
    // in the order of `keys`.
    std::vector<std::pair<KeyType, ValueType>> RemoveBatch(const std::vector<KeyType>& keys,
                                                           std::size_t index) {
        auto lock = acquire();
        auto it = storage.find(index);
        if (it == storage.end()) {
            return {};
        }
        auto removed = it->second.RemoveBulk(keys);
        eraseIfEmpty(it);
        return removed;
    }

    // Removes an arbitrary item from bucket `index` and returns it, or nullopt if the
    // bucket does not exist.
    std::optional<std::pair<KeyType, ValueType>> RemoveAny(std::size_t index) {
        auto lock = acquire();
        auto it = storage.find(index);
        if (it == storage.end()) {
            return std::nullopt;
        }
        auto removed = it->second.RemoveAny();
        eraseIfEmpty(it);
        return removed;
    }

    // Drops bucket `index` and returns everything it held (empty if it did not exist).
    std::vector<std::pair<KeyType, ValueType>> RemoveBucket(std::size_t index) {
        auto lock = acquire();
        auto it = storage.find(index);
        if (it == storage.end()) {
            return {};
        }
        auto removed = it->second.RemoveAll();
        storage.erase(it);
        return removed;
    }

    void Clear() {
        auto lock = acquire();
        storage.clear();
    }

    // Number of buckets that hold at least one item.
    [[nodiscard]]
    std::size_t Size() const {
        auto lock = acquire();
        return storage.size();
    }

    // Smallest index of a non-empty bucket, or nullopt if the map is empty.
    [[nodiscard]]
    std::optional<std::size_t> LowestIndex() const {
        auto lock = acquire();
        if (storage.empty()) {
            return std::nullopt;
        }
        return storage.begin()->first;
    }

    // Largest index of a non-empty bucket, or nullopt if the map is empty.
    [[nodiscard]]
    std::optional<std::size_t> HighestIndex() const {
        auto lock = acquire();
        if (storage.empty()) {
            return std::nullopt;
        }
        return storage.rbegin()->first;
    }

    // Serialization

    // Returns the buckets in ascending index order as
    // [{"index":I,"items":[{"key":K,"value":V},...]},...].
    [[nodiscard]]
    nlohmann::json ToJson() const
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        auto lock = acquire();
        nlohmann::json out = nlohmann::json::array();
        for (const auto& [index, items] : storage) {
            nlohmann::json entry = nlohmann::json::object();
            entry["index"] = index;
            entry["items"] = items.ToJson();
            out.push_back(std::move(entry));
        }
        return out;
    }

    // Reads buckets returned by ToJson and returns them as (index, items) pairs.
    static std::vector<std::pair<std::size_t, std::vector<std::pair<KeyType, ValueType>>>>
    ReadJson(const nlohmann::json& json)
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        std::vector<std::pair<std::size_t, std::vector<std::pair<KeyType, ValueType>>>> buckets;
        for (const auto& entry : detail::asArray(json)) {
            std::vector<std::pair<KeyType, ValueType>> items;
            for (const auto& item : detail::asArray(entry.at("items"))) {
                items.push_back(detail::keyValueFromJson<KeyType, ValueType>(item));
            }
            buckets.emplace_back(entry.at("index").get<std::size_t>(), std::move(items));
        }
        return buckets;
    }

    // Adds the buckets returned by ToJson. A key already in the same bucket gets the new
    // value. Throws nlohmann::json::exception if `json` does not have the expected shape,
    // and then nothing is added.
    void FromJson(const nlohmann::json& json)
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        auto buckets = ReadJson(json);

        auto lock = acquire();
        for (const auto& [index, items] : buckets) {
            if (!items.empty()) {
                storage[index].PutBulk(items);
            }
        }
    }
};

} // namespace JrCache
