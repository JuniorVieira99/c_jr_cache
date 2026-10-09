#pragma once

#include "utilities.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace JrCache {

// A type nlohmann::json can convert to and from. Numbers, bool, std::string and the standard
// containers work out of the box. For your own types, use NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE
// or write to_json/from_json functions (see the nlohmann/json documentation).
template <typename T>
concept JsonSerializable = requires(const T& value, const nlohmann::json& json) {
    nlohmann::json(value);
    json.template get<T>();
};

namespace detail {

inline constexpr std::pair<EvictionPolicy, std::string_view> policyNames[] = {
    {EvictionPolicy::LRU, "LRU"},   {EvictionPolicy::LFU, "LFU"},   {EvictionPolicy::MRU, "MRU"},
    {EvictionPolicy::MFU, "MFU"},   {EvictionPolicy::FIFO, "FIFO"}, {EvictionPolicy::LIFO, "LIFO"},
};

} // namespace detail

inline std::string_view PolicyName(EvictionPolicy policy) {
    for (const auto& [value, name] : detail::policyNames) {
        if (value == policy) return name;
    }
    return "unknown";
}

// Converts an EvictionPolicy to and from its name, such as "LRU". nlohmann::json finds these
// through argument-dependent lookup.
inline void to_json(nlohmann::json& json, EvictionPolicy policy) {
    json = std::string(PolicyName(policy));
}

inline void from_json(const nlohmann::json& json, EvictionPolicy& policy) {
    const auto& name = json.get_ref<const nlohmann::json::string_t&>();
    for (const auto& [value, policyName] : detail::policyNames) {
        if (policyName == name) {
            policy = value;
            return;
        }
    }
    throw std::invalid_argument("unknown eviction policy \"" + name + "\"");
}

namespace detail {

// Returns `json` as an array, or throws nlohmann::json::type_error if it is not one.
inline const nlohmann::json::array_t& asArray(const nlohmann::json& json) {
    return json.get_ref<const nlohmann::json::array_t&>();
}

// {"key":K,"value":V}
template <JsonSerializable K, JsonSerializable V>
nlohmann::json keyValueToJson(const K& key, const V& value) {
    nlohmann::json item = nlohmann::json::object();
    item["key"] = key;
    item["value"] = value;
    return item;
}

template <JsonSerializable K, JsonSerializable V>
std::pair<K, V> keyValueFromJson(const nlohmann::json& item) {
    return {item.at("key").get<K>(), item.at("value").get<V>()};
}

} // namespace detail

} // namespace JrCache
