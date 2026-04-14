#pragma once
#include "types.hpp"
#include <unordered_map>

namespace tiny_agent {

struct ToolSchema {
    std::string name;
    std::string description;
    json parameters = json::object();
};

struct Tool {
    ToolSchema                       schema;
    std::function<json(const json&)> fn;

    json operator()(const json& args) const {
        if (!fn) throw ToolError("tool '" + schema.name + "' has no handler");
        return fn(args);
    }

    template<std::invocable<const json&> Fn>
    static Tool create(std::string name, std::string desc, Fn&& f,
                       json params = json::object()) {
        return {{std::move(name), std::move(desc), std::move(params)},
                std::forward<Fn>(f)};
    }

    template<typename Ret, typename Fn>
        requires std::invocable<Fn, const json&> &&
                 std::convertible_to<std::invoke_result_t<Fn, const json&>, json>
    static Tool typed(std::string name, std::string desc, Fn&& f,
                      json params = json::object()) {
        return create(std::move(name), std::move(desc),
            [fn = std::forward<Fn>(f)](const json& args) -> json {
                return json(fn(args));
            },
            std::move(params));
    }
};

class ToolRegistry {
    std::unordered_map<std::string, Tool> tools_;
public:
    void add(Tool t) { tools_[t.schema.name] = std::move(t); }

    [[nodiscard]] const Tool& get(const std::string& name) const {
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
