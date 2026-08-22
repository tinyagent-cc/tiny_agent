#pragma once
#include "../core/tool.hpp"
#include "../integrations/rete_convert.hpp"
#include <rete/rete.hpp>

namespace tiny_agent::tools {

struct ReteToolConfig {
    std::string name = "expert_system";
    std::string description = "Evaluate facts against a rule base and return derived facts.";
    std::function<void(rete::ReteEngine&)> setup;   // registers rules on a fresh engine
    int max_cycles = 256;
};

// A fresh engine per invocation keeps the tool deterministic and stateless;
// rule bases are cheap to rebuild at this scale.
inline DynamicTool rete_tool(ReteToolConfig cfg) {
    if (!cfg.setup) throw ToolError("rete_tool: setup is required");
    json params = {
        {"type", "object"},
        {"properties", {{"facts", {
            {"type", "array"},
            {"description", "Triples [id, attribute, value]"},
            {"items", {{"type", "array"}}}}}}},
        {"required", {"facts"}}};

    return DynamicTool::create(cfg.name, cfg.description,
        [cfg](const json& args) -> json {
            rete::ReteEngine eng;
            cfg.setup(eng);
            for (auto& f : args.value("facts", json::array())) {
                if (!f.is_array() || f.size() != 3)
                    throw ToolError("rete_tool: each fact must be [id, attr, value]");
                eng.assert_fact(integrations::to_rete_value(f[0]),
                                integrations::to_rete_value(f[1]),
                                integrations::to_rete_value(f[2]));
            }
            eng.run(cfg.max_cycles);
            json out = json::array();
            for (auto& w : eng.facts())
                out.push_back({integrations::from_rete_value(w->identifier),
                               integrations::from_rete_value(w->attribute),
                               integrations::from_rete_value(w->value)});
            return json{{"facts", out}};
        },
        std::move(params));
}

} // namespace tiny_agent::tools
