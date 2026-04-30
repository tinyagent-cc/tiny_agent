#pragma once
#include "store.hpp"
#include "../core/tool.hpp"
#include <memory>

namespace tiny_agent::memory {

// ── ToolCache<Store> — deterministic tool-result cache ──────────────────────
//
// Caches the JSON result of tool calls keyed by (name, arguments).
// Only wrap tools whose output is deterministic for a given input —
// side-effectful tools (file writes, API mutations) should not be cached.
//
// Uses the memory_store concept so the backing store is swappable via
// template specialization (InMemoryStore, file-backed, etc.).

template<memory_store Store = InMemoryStore<256>>
class ToolCache {
    Store store_;

    static std::string make_key(const std::string& name, const json& args) {
        return name + '\0' + args.dump(-1, ' ', false,
                                        json::error_handler_t::replace);
    }

public:
    ToolCache() = default;
    explicit ToolCache(Store s) : store_(std::move(s)) {}

    std::optional<std::string> lookup(const std::string& name,
                                      const json& args) {
        return store_.get(make_key(name, args));
    }

    void store(const std::string& name, const json& args,
               const std::string& result) {
        store_.put(make_key(name, args), result);
    }

    bool has(const std::string& name, const json& args) const {
        return store_.has(make_key(name, args));
    }

    void invalidate(const std::string& name, const json& args) {
        store_.remove(make_key(name, args));
    }

    void clear()          { store_.clear(); }
    std::size_t size() const { return store_.size(); }
    Store& backing_store()   { return store_; }
};

// ── cached() — wrap a DynamicTool with transparent result caching ────────────
//
// Returns a new DynamicTool with the same schema whose handler checks the cache
// before executing.  The cache is shared (ref-counted) so the DynamicTool's
// std::function stays copyable.
//
//   auto my_tool = DynamicTool::create("search", "web search", search_fn);
//   auto fast    = memory::cached<128>(std::move(my_tool));  // LRU-128

template<std::size_t MaxEntries = 256>
DynamicTool cached(DynamicTool tool) {
    auto cache = std::make_shared<ToolCache<InMemoryStore<MaxEntries>>>();
    auto name  = tool.schema.name;

    return DynamicTool::create(
        tool.schema.name, tool.schema.description,
        [fn = std::move(tool.fn), cache, name](const json& args) -> json {
            if (auto hit = cache->lookup(name, args))
                return json::parse(*hit);
            auto result = fn(args);
            cache->store(name, args, result.dump());
            return result;
        },
        tool.schema.parameters);
}

// Overload accepting a pre-existing shared cache (for cross-tool sharing).
template<memory_store Store>
DynamicTool cached(DynamicTool tool, std::shared_ptr<ToolCache<Store>> cache) {
    auto name = tool.schema.name;

    return DynamicTool::create(
        tool.schema.name, tool.schema.description,
        [fn = std::move(tool.fn), cache, name](const json& args) -> json {
            if (auto hit = cache->lookup(name, args))
                return json::parse(*hit);
            auto result = fn(args);
            cache->store(name, args, result.dump());
            return result;
        },
        tool.schema.parameters);
}

} // namespace tiny_agent::memory
