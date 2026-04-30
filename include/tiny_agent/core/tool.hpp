#pragma once
#include "types.hpp"
#include <unordered_map>
#include <concepts>

namespace tiny_agent {

// ─── ToolSchema ─────────────────────────────────────────────────────────────

struct ToolSchema {
    std::string name;
    std::string description;
    json parameters = json::object();
};

// ─── Tool concept (compile-time checked tools) ─────────────────────────────
//
// A Tool must expose:
//   name()        → string
//   description() → string
//   parameters()  → json     (JSON schema for arguments)
//   invoke(json)  → json     (executes the tool)

template<typename T>
concept Tool =
    requires(T t, const json& args) {
        { t.name()        } -> std::convertible_to<std::string>;
        { t.description() } -> std::convertible_to<std::string>;
        { t.parameters()  } -> std::convertible_to<json>;
        { t.invoke(args)  } -> std::convertible_to<json>;
    };

// ─── DynamicTool (runtime callable wrapper, formerly v1's Tool struct) ──────

struct DynamicTool {
    ToolSchema                       schema;
    std::function<json(const json&)> fn;

    json operator()(const json& args) const {
        if (!fn) throw ToolError("tool '" + schema.name + "' has no handler");
        return fn(args);
    }

    template<std::invocable<const json&> Fn>
    static DynamicTool create(std::string name, std::string desc, Fn&& f,
                              json params = json::object()) {
        return {{std::move(name), std::move(desc), std::move(params)},
                std::forward<Fn>(f)};
    }

    template<typename Ret, typename Fn>
        requires std::invocable<Fn, const json&> &&
                 std::convertible_to<std::invoke_result_t<Fn, const json&>, json>
    static DynamicTool typed(std::string name, std::string desc, Fn&& f,
                             json params = json::object()) {
        return create(std::move(name), std::move(desc),
            [fn = std::forward<Fn>(f)](const json& args) -> json {
                return json(fn(args));
            },
            std::move(params));
    }
};

// ─── to_dynamic_tool: convert a concept Tool to DynamicTool ────────────────

template<Tool T>
DynamicTool to_dynamic_tool(T tool) {
    auto name = std::string(tool.name());
    auto desc = std::string(tool.description());
    auto params = json(tool.parameters());
    return DynamicTool::create(
        std::move(name), std::move(desc),
        [t = std::move(tool)](const json& args) -> json {
            return t.invoke(args);
        },
        std::move(params));
}

// ─── ToolRegistry (runtime tool lookup) ─────────────────────────────────────

class ToolRegistry {
    std::unordered_map<std::string, DynamicTool> tools_;
public:
    void add(DynamicTool t) { tools_[t.schema.name] = std::move(t); }

    template<Tool T>
    void add(T tool) { add(to_dynamic_tool(std::move(tool))); }

    [[nodiscard]] const DynamicTool& get(const std::string& name) const {
        auto it = tools_.find(name);
        if (it == tools_.end()) throw ToolError("unknown tool: " + name);
        return it->second;
    }

    [[nodiscard]] bool has(const std::string& name) const { return tools_.count(name); }

    [[nodiscard]] std::vector<ToolSchema> schemas() const {
        std::vector<ToolSchema> out;
        out.reserve(tools_.size());
        for (auto& [_, t] : tools_) out.push_back(t.schema);
        return out;
    }

    json execute(const std::string& name, const json& args) const {
        return get(name)(args);
    }

    auto begin() const { return tools_.begin(); }
    auto end()   const { return tools_.end(); }
    auto size()  const { return tools_.size(); }
};

} // namespace tiny_agent
