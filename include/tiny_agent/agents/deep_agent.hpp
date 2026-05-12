#pragma once
#include "base.hpp"
#include "../core/parser.hpp"
#include <memory>

namespace tiny_agent::agents {

template<is_chat LLMType>
class DeepAgent : public std::enable_shared_from_this<DeepAgent<LLMType>> {
    LLMType     llm_;
    AgentConfig cfg_;
    Log&        log_;
    ToolRegistry    registry_;
    MiddlewareChain chain_;
    std::vector<Message> history_;

    LLMResponse call_llm(std::vector<Message>& msgs, const LLMConfig& overrides = {}) {
        auto schemas = registry_.schemas();
        log_.trace(cfg_.name, "sending " + std::to_string(msgs.size())
            + " messages with " + std::to_string(schemas.size()) + " tool schemas");
        auto effective = LLMConfig::merge(cfg_.llm_config, overrides);
        auto terminal = [this, &schemas](std::vector<Message>& m) -> LLMResponse {
            return llm_.chat(m, schemas);
        };
        if (chain_.empty()) return terminal(msgs);
        log_.trace(cfg_.name, "running through middleware chain");
        return chain_.run(msgs, terminal);
    }

    std::vector<Message> execute_tools(const std::vector<ToolCall>& calls) {
        std::vector<Message> results;
        for (auto& tc : calls) {
            log_.info(cfg_.name, "calling tool: " + tc.name);
            log_.trace(cfg_.name, "tool args [" + tc.name + "]: " + tc.arguments.dump());
            try {
                auto result = registry_.execute(tc.name, tc.arguments);
                std::string body = result.is_string()
                    ? result.template get<std::string>() : result.dump();
                log_.trace(cfg_.name, "tool result [" + tc.name + "]: " + body);
                auto msg = Message::tool_result(tc.id, std::move(body));
                msg.name = tc.name;
                results.push_back(std::move(msg));
            } catch (const std::exception& e) {
                log_.error(cfg_.name, std::string("tool error [") + tc.name + "]: " + e.what());
                json err;
                err["error"] = e.what();
                auto msg = Message::tool_result(tc.id, err.dump());
                msg.name = tc.name;
                results.push_back(std::move(msg));
            }
        }
        return results;
    }

public:
    using input_t  = std::string;
    using output_t = std::string;

    DeepAgent(LLMType llm, AgentConfig cfg = {})
        : llm_(std::move(llm)), cfg_(std::move(cfg)), log_(cfg_.logger)
    {
        log_.debug(cfg_.name, "initializing (max_iterations=" + std::to_string(cfg_.max_iterations)
            + " tools=" + std::to_string(cfg_.tools.size())
            + " middlewares=" + std::to_string(cfg_.middlewares.size()) + ")");
        for (auto& t : cfg_.tools) {
            log_.trace(cfg_.name, "registering tool: " + t.schema.name);
            registry_.add(t);
        }
        for (auto& m : cfg_.middlewares) chain_.add(m);
    }

    std::string execute_loop(std::vector<Message>& msgs, const LLMConfig& overrides,
                             const char* label = "run") {
        for (int i = 0; i < cfg_.max_iterations; ++i) {
            log_.debug(cfg_.name, std::string(label) + " iteration " + std::to_string(i + 1)
                + "/" + std::to_string(cfg_.max_iterations)
                + " (messages=" + std::to_string(msgs.size()) + ")");
            auto resp = call_llm(msgs, overrides);
            msgs.push_back(resp.message);

            if (!resp.message.has_tool_calls()) {
                log_.debug(cfg_.name, "done: " + resp.finish_reason);
                return resp.message.text();
            }

            log_.debug(cfg_.name, "LLM requested " + std::to_string(resp.message.tool_calls.size()) + " tool call(s)");
            auto tool_results = execute_tools(resp.message.tool_calls);
            for (auto& tr : tool_results)
                msgs.push_back(std::move(tr));
        }
        log_.warn(cfg_.name, "reached max iterations (" + std::to_string(cfg_.max_iterations) + ")");
        return "Error: agent reached maximum iterations ("
            + std::to_string(cfg_.max_iterations) + ")";
    }

    std::string invoke(const std::string& input, const LLMConfig& overrides = {}) {
        return invoke(input, RunConfig{}, overrides);
    }

    std::string invoke(const std::string& input, const RunConfig&, const LLMConfig& overrides = {}) {
        log_.debug(cfg_.name, "invoke(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        std::vector<Message> msgs;
        if (!cfg_.system_prompt.empty())
            msgs.push_back(Message::system(cfg_.system_prompt));
        msgs.push_back(Message::user(input));
        return execute_loop(msgs, overrides);
    }

    std::string chat(const std::string& input, const LLMConfig& overrides = {}) {
        log_.debug(cfg_.name, "chat(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        if (history_.empty() && !cfg_.system_prompt.empty())
            history_.push_back(Message::system(cfg_.system_prompt));
        history_.push_back(Message::user(input));
        return execute_loop(history_, overrides, "chat");
    }

    // Compatibility alias for invoke
    std::string run(const std::string& input, const LLMConfig& overrides = {}) {
        return invoke(input, overrides);
    }

    template<output_parser Parser = TextParser>
    auto invoke_parsed(const std::string& input, const LLMConfig& overrides = {})
        -> typename Parser::output_type
    {
        auto text = invoke(input, overrides);
        LLMResponse synthetic{Message::assistant(text), {}, "stop", {}};
        return Parser::parse(synthetic);
    }

    void add_tool(DynamicTool t) {
        log_.debug(cfg_.name, "adding tool: " + t.schema.name);
        cfg_.tools.push_back(t);
        registry_.add(std::move(t));
    }

    template<Tool T>
    void add_tool(T tool) { add_tool(to_dynamic_tool(std::move(tool))); }

    DynamicTool as_tool(std::string name, std::string description,
                        json params = json()) {
        if (params.empty())
            params = {{"type", "object"},
                      {"properties", {{"input", {{"type", "string"},
                                                  {"description", "The task to delegate"}}}}},
                      {"required", {"input"}}};
        auto self = this->shared_from_this();
        return DynamicTool::create(
            std::move(name), std::move(description),
            [self](const json& args) -> json {
                auto input = args.contains("input")
                    ? args["input"].template get<std::string>() : args.dump();
                return json(self->invoke(input));
            },
            std::move(params));
    }

    std::vector<std::string> batch(std::vector<std::string> inputs, const RunConfig& cfg = {}) {
        std::vector<std::string> out;
        out.reserve(inputs.size());
        for (auto& i : inputs) out.push_back(invoke(std::move(i), cfg));
        return out;
    }

    void stream(const std::string& input, std::function<void(std::string)> cb,
                const RunConfig& = {}) {
        cb(invoke(input));
    }

    void clear_history() { history_.clear(); }
    const std::vector<Message>& history() const { return history_; }
    const AgentConfig& agent_config() const { return cfg_; }
    LLMType& llm() { return llm_; }
    const LLMType& llm() const { return llm_; }
    Log& log() { return log_; }
    const Log& log() const { return log_; }
    std::size_t tool_count() const { return registry_.size(); }
    std::vector<ToolSchema> tool_schemas() const { return registry_.schemas(); }
};

template<is_chat L>
DeepAgent(L, AgentConfig) -> DeepAgent<L>;

template<is_chat L>
DeepAgent(L) -> DeepAgent<L>;

} // namespace tiny_agent::agents
