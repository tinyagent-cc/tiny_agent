#pragma once
#include <concepts>
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace tiny_agent::memory {

// ── Concept: key-value memory store ─────────────────────────────────────────
//
// Minimal interface for a string→string store with bounded capacity.
// Specialize InMemoryStore for constrained hardware; implement the concept
// for file-backed or networked stores.

template<typename T>
concept memory_store = requires(T& s, const std::string& k, const std::string& v) {
    { s.get(k) }    -> std::same_as<std::optional<std::string>>;
    { s.has(k) }    -> std::convertible_to<bool>;
    { s.size() }    -> std::convertible_to<std::size_t>;
    s.put(k, v);
    s.remove(k);
    s.clear();
};

// ── InMemoryStore<N> — bounded LRU key-value store ──────────────────────────
//
// O(1) get / put / remove via std::list + std::unordered_map.
// Evicts least-recently-used entry when capacity is reached.
// MaxEntries is a compile-time bound to prevent unbounded growth on
// constrained hardware.

template<std::size_t MaxEntries = 128>
class InMemoryStore {
    static_assert(MaxEntries > 0, "MaxEntries must be positive");

    struct Entry { std::string key; std::string value; };
    using List    = std::list<Entry>;
    using ListIt  = typename List::iterator;
    using Index   = std::unordered_map<std::string, ListIt>;

    List  entries_;   // front = most recently used
    Index index_;

public:
    InMemoryStore() { index_.reserve(MaxEntries); }

    std::optional<std::string> get(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        entries_.splice(entries_.begin(), entries_, it->second);
        return it->second->value;
    }

    void put(const std::string& key, const std::string& value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = value;
            entries_.splice(entries_.begin(), entries_, it->second);
            return;
        }
        if (entries_.size() >= MaxEntries) {
            index_.erase(entries_.back().key);
            entries_.pop_back();
        }
        entries_.push_front({key, value});
        index_[key] = entries_.begin();
    }

    bool has(const std::string& key) const { return index_.count(key) > 0; }

    void remove(const std::string& key) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            entries_.erase(it->second);
            index_.erase(it);
        }
    }

    void clear() { entries_.clear(); index_.clear(); }

    std::size_t size() const { return entries_.size(); }

    static constexpr std::size_t capacity() { return MaxEntries; }
};

static_assert(memory_store<InMemoryStore<>>);

} // namespace tiny_agent::memory
