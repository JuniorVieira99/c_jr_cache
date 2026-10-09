#pragma once

#include "cache_serialization.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <type_traits>
#include <mutex>
#include <vector>

namespace JrCache {

// A list node. It holds a non-owning pointer to the value: whoever inserts the
// value keeps ownership, and the Remove* functions hand the pointer back.
template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible<KeyType>::value &&
             std::is_copy_constructible<ValueType>::value && (sizeof(KeyType) > 0)
class node{

    KeyType key;
    ValueType* value;
    node* next;
    node* prev;

public:

    node(const KeyType& k, ValueType* v) : key(k), value(v), next(nullptr), prev(nullptr) {}
    node(const node<KeyType, ValueType>& o) : key(o.key), value(o.value), next(o.next), prev(o.prev) {}
    node(node<KeyType, ValueType>&& o) noexcept : key(std::move(o.key)), value(o.value), next(o.next), prev(o.prev) {
        o.value = nullptr;
        o.next = nullptr;
        o.prev = nullptr;
    }
    node& operator=(const node<KeyType, ValueType>& o) {
        if (this != &o) {
            key = o.key;
            value = o.value;
            next = o.next;
            prev = o.prev;
        }
        return *this;
    }
    ~node() {
        next = nullptr;
        prev = nullptr;
    }

    const KeyType& getKey() const { return key; }
    ValueType* getValue() const { return value; }
    node* getNext() const { return next; }
    node* getPrev() const { return prev; }

    void setValue(ValueType* v) { value = v; }
    void setNext(node* n) { next = n; }
    void setPrev(node* p) { prev = p; }
};

template <typename KeyType, typename ValueType>
    requires std::is_copy_constructible<KeyType>::value &&
             std::is_copy_constructible<ValueType>::value && (sizeof(KeyType) > 0)
class LinkedList{

    uint64_t current_size{0};
    node<KeyType, ValueType>* head;
    node<KeyType, ValueType>* tail;
    std::unordered_map<KeyType, node<KeyType, ValueType>*> node_map;

    const bool Locked;
    mutable std::mutex mtx;

    bool containsNode(const KeyType& key) const {
        return node_map.find(key) != node_map.end();
    }

    // The three functions below only re-link pointers. node_map and current_size are
    // updated separately, when a node is created or destroyed, so moving a node that is
    // already in the list costs no hashing and no allocation.

    void unlinkNode(node<KeyType, ValueType>* n) {
        if (!n) return;
        if (n->getPrev()) {
            n->getPrev()->setNext(n->getNext());
        } else {
            head = n->getNext();
        }
        if (n->getNext()) {
            n->getNext()->setPrev(n->getPrev());
        } else {
            tail = n->getPrev();
        }
        n->setNext(nullptr);
        n->setPrev(nullptr);
    }

    void linkNodeAtHead(node<KeyType, ValueType>* n) {
        if (!n) return;
        n->setNext(head);
        n->setPrev(nullptr);
        if (head) {
            head->setPrev(n);
        }
        head = n;
        if (!tail) {
            tail = n;
        }
    }

    void linkNodeAtTail(node<KeyType, ValueType>* n) {
        if (!n) return;
        n->setPrev(tail);
        n->setNext(nullptr);
        if (tail) {
            tail->setNext(n);
        }
        tail = n;
        if (!head) {
            head = n;
        }
    }

    // Locks the mutex only when this list was created as thread-safe. The
    // returned lock releases it on scope exit, including when an exception is thrown.
    [[nodiscard]]
    std::unique_lock<std::mutex> acquire() const {
        if (!Locked) return {};
        return std::unique_lock<std::mutex>(mtx);
    }

    // Creates a node for a key that is not in the list yet and records it in node_map.
    // The node is not linked. Caller holds the lock.
    node<KeyType, ValueType>* createNode(const KeyType& key, ValueType* value) {
        auto owned = std::make_unique<node<KeyType, ValueType>>(key, value);
        node_map.emplace(key, owned.get()); // if this throws, `owned` frees the node
        ++current_size;
        return owned.release();
    }

    // Inserts or updates `key` and moves it to the head. Caller holds the lock.
    void setTop(const KeyType& key, ValueType* value) {
        auto it = node_map.find(key);
        if (it != node_map.end()) {
            node<KeyType, ValueType>* n = it->second;
            n->setValue(value);
            if (n != head) {
                unlinkNode(n);
                linkNodeAtHead(n);
            }
        } else {
            linkNodeAtHead(createNode(key, value));
        }
    }

    // Inserts or updates `key` and moves it to the tail. Caller holds the lock.
    void setBottom(const KeyType& key, ValueType* value) {
        auto it = node_map.find(key);
        if (it != node_map.end()) {
            node<KeyType, ValueType>* n = it->second;
            n->setValue(value);
            if (n != tail) {
                unlinkNode(n);
                linkNodeAtTail(n);
            }
        } else {
            linkNodeAtTail(createNode(key, value));
        }
    }

    // Unlinks and frees the node `it` points to, returning the value pointer it held.
    // Caller holds the lock.
    ValueType* removeNode(typename std::unordered_map<KeyType, node<KeyType, ValueType>*>::iterator it) {
        node<KeyType, ValueType>* n = it->second;
        node_map.erase(it);
        --current_size;
        unlinkNode(n);
        ValueType* value = n->getValue();
        delete n;
        return value;
    }

    // Same, for a node reached through head or tail.
    ValueType* removeNode(node<KeyType, ValueType>* n) {
        return removeNode(node_map.find(n->getKey()));
    }

    void deleteAllNodes() {
        node<KeyType, ValueType>* current = head;
        while (current) {
            node<KeyType, ValueType>* next = current->getNext();
            delete current;
            current = next;
        }
        head = nullptr;
        tail = nullptr;
        node_map.clear();
        current_size = 0;
    }

public:
    explicit LinkedList(bool lock = true) : head(nullptr), tail(nullptr), Locked(lock) {}
    ~LinkedList() {
        deleteAllNodes();
    }

    [[nodiscard]]
    uint64_t GetSize() const {
        auto guard = acquire();
        return current_size;
    }
    [[nodiscard]]
    bool IsEmpty() const {
        auto guard = acquire();
        return current_size == 0;
    }

    [[nodiscard]]
    bool ContainsNode(const KeyType& key) const {
        auto guard = acquire();
        return containsNode(key);
    }

    void SetTop(const KeyType& key, ValueType* value) {
        if (!value) return;
        auto guard = acquire();
        setTop(key, value);
    }

    void SetBottom(const KeyType& key, ValueType* value) {
        if (!value) return;
        auto guard = acquire();
        setBottom(key, value);
    }

    std::optional<ValueType*> Remove(const KeyType& key) {
        auto guard = acquire();
        auto it = node_map.find(key);
        if (it == node_map.end()) return std::nullopt;
        return removeNode(it);
    }

    std::optional<ValueType*> RemoveTop() {
        auto guard = acquire();
        if (!head) return std::nullopt;
        return removeNode(head);
    }

    std::optional<ValueType*> RemoveBottom() {
        auto guard = acquire();
        if (!tail) return std::nullopt;
        return removeNode(tail);
    }

    [[nodiscard]]
    std::optional<ValueType> GetTop() const {
        auto guard = acquire();
        if (!head) return std::nullopt;
        return *(head->getValue());
    }

    [[nodiscard]]
    std::optional<ValueType> GetBottom() const {
        auto guard = acquire();
        if (!tail) return std::nullopt;
        return *(tail->getValue());
    }

    [[nodiscard]]
    std::optional<KeyType> GetTopKey() const {
        auto guard = acquire();
        if (!head) return std::nullopt;
        return head->getKey();
    }

    [[nodiscard]]
    std::optional<KeyType> GetBottomKey() const {
        auto guard = acquire();
        if (!tail) return std::nullopt;
        return tail->getKey();
    }


    void Clear() {
        auto guard = acquire();
        deleteAllNodes();
    }

    // Batch operations

    // Same as calling SetTop for each item in order, but under a single lock.
    // The last item ends up at the head. Null values are skipped.
    void SetBatchTop(const std::vector<std::pair<KeyType, ValueType*>>& items) {
        auto guard = acquire();
        for (const auto& [key, value] : items) {
            if (value) setTop(key, value);
        }
    }

    // Same as calling SetBottom for each item in order, but under a single lock.
    // The last item ends up at the tail. Null values are skipped.
    void SetBatchBottom(const std::vector<std::pair<KeyType, ValueType*>>& items) {
        auto guard = acquire();
        for (const auto& [key, value] : items) {
            if (value) setBottom(key, value);
        }
    }

    std::vector<std::optional<ValueType>> GetBatch(const std::vector<KeyType>& keys) const {
        std::vector<std::optional<ValueType>> values;
        values.reserve(keys.size());

        auto guard = acquire();
        for (const auto& key : keys) {
            auto it = node_map.find(key);
            if (it != node_map.end()) {
                values.push_back(*(it->second->getValue()));
            } else {
                values.push_back(std::nullopt);
            }
        }
        return values;
    }

    std::vector<ValueType*> RemoveBatch(const std::vector<KeyType>& keys) {
        std::vector<ValueType*> removed_values;
        auto guard = acquire();
        for (const auto& key : keys) {
            auto it = node_map.find(key);
            if (it != node_map.end()) {
                removed_values.push_back(removeNode(it));
            }
        }
        return removed_values;
    }

    // Serialization

    // Returns the list from top to bottom as [{"key":K,"value":V},...].
    [[nodiscard]]
    nlohmann::json ToJson() const
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        auto guard = acquire();
        nlohmann::json out = nlohmann::json::array();
        for (node<KeyType, ValueType>* current = head; current; current = current->getNext()) {
            out.push_back(detail::keyValueToJson(current->getKey(), *(current->getValue())));
        }
        return out;
    }

    // Reads a list returned by ToJson and returns its items from top to bottom.
    static std::vector<std::pair<KeyType, ValueType>> ReadJson(const nlohmann::json& json)
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType>
    {
        std::vector<std::pair<KeyType, ValueType>> items;
        for (const auto& item : detail::asArray(json)) {
            items.push_back(detail::keyValueFromJson<KeyType, ValueType>(item));
        }
        return items;
    }

    // Adds the items of a list returned by ToJson, keeping their order. With `putAtTop` they
    // go above the existing nodes, otherwise below them. A key that is already in the list
    // is moved and gets the new value, as with SetTop and SetBottom.
    //
    // The list does not own its values, so `store(key, value)` is called for every item
    // (without the list's lock held). It must keep the value alive, for example in a
    // container the caller owns, and return a pointer to it; items it returns nullptr for
    // are skipped. Throws nlohmann::json::exception if `json` does not have the expected
    // shape, and then nothing is added.
    template <typename Store>
        requires JsonSerializable<KeyType> && JsonSerializable<ValueType> &&
                 std::is_invocable_r_v<ValueType*, Store&, const KeyType&, ValueType&&>
    void FromJson(const nlohmann::json& json, Store&& store, bool putAtTop = false) {
        std::vector<std::pair<KeyType, ValueType>> items = ReadJson(json);

        std::vector<std::pair<KeyType, ValueType*>> stored;
        stored.reserve(items.size());
        for (auto& [key, value] : items) {
            stored.emplace_back(key, store(key, std::move(value)));
        }

        auto guard = acquire();
        if (putAtTop) {
            for (auto it = stored.rbegin(); it != stored.rend(); ++it) {
                if (it->second) setTop(it->first, it->second);
            }
        } else {
            for (const auto& [key, value] : stored) {
                if (value) setBottom(key, value);
            }
        }
    }

};

} // namespace JrCache
