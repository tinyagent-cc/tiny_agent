#pragma once
#include "base.hpp"
#include "deep_agent.hpp"
#include <memory>

namespace tiny_agent {

template<typename Tag>
struct create_agent;

template<>
struct create_agent<agents::deep_agent_tag> {
    template<is_chat LLM>
    struct Params {
        LLM llm;
        std::string name = "agent";
        std::string system_prompt;
        std::vector<DynamicTool> tools;
        int max_iterations = 10;
        LLMConfig llm_config;
        Log logger;
        json kwargs = json::object();
    };

    template<is_chat LLM>
    static auto create(Params<LLM> p) {
        agents::AgentConfig cfg;
        cfg.name = std::move(p.name);
        cfg.system_prompt = std::move(p.system_prompt);
        cfg.tools = std::move(p.tools);
        cfg.max_iterations = p.max_iterations;
        cfg.llm_config = std::move(p.llm_config);
        cfg.logger = std::move(p.logger);
        cfg.kwargs = std::move(p.kwargs);
        return agents::DeepAgent<LLM>(std::move(p.llm), std::move(cfg));
    }

    template<is_chat LLM>
    static auto create_shared(Params<LLM> p) {
        agents::AgentConfig cfg;
        cfg.name = std::move(p.name);
        cfg.system_prompt = std::move(p.system_prompt);
        cfg.tools = std::move(p.tools);
        cfg.max_iterations = p.max_iterations;
        cfg.llm_config = std::move(p.llm_config);
        cfg.logger = std::move(p.logger);
        cfg.kwargs = std::move(p.kwargs);
        return std::make_shared<agents::DeepAgent<LLM>>(
            std::move(p.llm), std::move(cfg));
    }
};

template<is_chat LLM>
auto make_agent(LLM llm = {}, agents::AgentConfig cfg = {}) {
    return agents::DeepAgent(std::move(llm), std::move(cfg));
}

template<is_chat LLM>
auto make_shared_agent(LLM llm = {}, agents::AgentConfig cfg = {}) {
    return std::make_shared<agents::DeepAgent<LLM>>(std::move(llm), std::move(cfg));
}

template<typename AgentType>
DynamicTool agent_as_tool(std::shared_ptr<AgentType> agent,
                          std::string name, std::string description,
                          json params = json()) {
    if (params.empty())
        params = {{"type", "object"},
                  {"properties", {{"input", {{"type", "string"},
                                              {"description", "The task to delegate"}}}}},
                  {"required", {"input"}}};

    return DynamicTool::create(
        std::move(name), std::move(description),
        [agent = std::move(agent)](const json& args) -> json {
            auto input = args.contains("input")
                ? args["input"].get<std::string>() : args.dump();
            return json(agent->invoke(input));
        },
        std::move(params));
}

template<typename LLMType>
    requires is_chat<LLMType>
struct BindedModel {
    LLMType llm_;
    LLMConfig overrides_;

public:
    using model_tag = chat_tag;
    using input_t   = std::string;
    using output_t  = std::string;

    BindedModel(LLMType llm, LLMConfig overrides)
        : llm_(std::move(llm)), overrides_(std::move(overrides)) {}

    std::string invoke(std::string prompt, const RunConfig& = {}) {
        std::vector<Message> msgs = {Message::user(std::move(prompt))};
        return chat(msgs).message.text();
    }

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {}) {
        return llm_.chat(msgs, tools);
    }

    std::string model_name() const { return llm_.model_name(); }
    float get_temperature() const {
        return overrides_.temperature.value_or(llm_.get_temperature());
    }

    std::vector<std::string> batch(std::vector<std::string> prompts, const RunConfig& = {}) {
        std::vector<std::string> out;
        out.reserve(prompts.size());
        for (auto& p : prompts) out.push_back(invoke(std::move(p)));
        return out;
    }

    void stream(std::string prompt, std::function<void(std::string)> cb, const RunConfig& = {}) {
        cb(invoke(std::move(prompt)));
    }

    const LLMConfig& overrides() const { return overrides_; }
};

template<is_chat LLM>
auto bind(LLM llm, LLMConfig overrides) {
    return BindedModel<std::decay_t<LLM>>(std::move(llm), std::move(overrides));
}

} // namespace tiny_agent
