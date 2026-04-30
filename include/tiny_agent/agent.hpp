#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  agent.hpp  —  AgentExecutor with strategy-based partial specialization
//
//  Primary template  AgentExecutor<Strategy, LLM>  is undefined.
//  Partial specialization  AgentExecutor<deep_agent_tag, LLM>  implements
//  a ReAct-style think→act→observe loop, constrained via  requires is_chat<LLM>.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/types.hpp"
#include "core/tool.hpp"
#include "core/model.hpp"
#include "core/middleware.hpp"
#include "core/log.hpp"
#include "core/parser.hpp"
#include <memory>

namespace tiny_agent {

// ─── Strategy tags ────────────────────────────────────────────────────────────

struct deep_agent_tag {};

// ─── Agent configuration ──────────────────────────────────────────────────────

struct AgentConfig {
    std::string name = "agent";
    std::string system_prompt;
    std::vector<DynamicTool>   tools;
    std::vector<MiddlewareFn>  middlewares;
    int max_iterations = 10;
};

// ─── Primary template — undefined: forces use of a strategy specialization ───

template<typename Strategy, typename LLM>
class AgentExecutor;

// ═══════════════════════════════════════════════════════════════════════════════
//  Partial specialization:  AgentExecutor<deep_agent_tag, LLM>
//
//  Constraint:  LLM must satisfy is_chat
// ═══════════════════════════════════════════════════════════════════════════════

template<is_chat LLMType>
class AgentExecutor<deep_agent_tag, LLMType>
    : public std::enable_shared_from_this<AgentExecutor<deep_agent_tag, LLMType>> {

    AgentConfig     cfg_;
    LLMType         llm_;
    Log             log_;
    ToolRegistry    registry_;
    MiddlewareChain chain_;
    std::vector<Message> history_;

    LLMResponse call_llm(std::vector<Message>& msgs) {
        auto schemas = registry_.schemas();
        log_.trace(cfg_.name, "sending " + std::to_string(msgs.size())
            + " messages with " + std::to_string(schemas.size()) + " tool schemas");
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

    AgentExecutor(LLMType llm, AgentConfig cfg = {}, Log log = {})
        : cfg_(std::move(cfg)), llm_(std::move(llm)), log_(std::move(log))
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

    // ── Core loop ─────────────────────────────────────────────────────────

    std::string execute_loop(std::vector<Message>& msgs, const char* label = "run") {
        for (int i = 0; i < cfg_.max_iterations; ++i) {
            log_.debug(cfg_.name, std::string(label) + " iteration " + std::to_string(i + 1)
                + "/" + std::to_string(cfg_.max_iterations)
                + " (messages=" + std::to_string(msgs.size()) + ")");
            auto resp = call_llm(msgs);
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
        log_.warn(cfg_.name, std::string(label) + " reached max iterations ("
            + std::to_string(cfg_.max_iterations) + ")");
        return "Error: agent reached maximum iterations ("
            + std::to_string(cfg_.max_iterations) + ") without producing a final response.";
    }

    // ── Single-shot execution ─────────────────────────────────────────────

    std::string run(const std::string& input) {
        log_.debug(cfg_.name, "run(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        std::vector<Message> msgs;
        if (!cfg_.system_prompt.empty())
            msgs.push_back(Message::system(cfg_.system_prompt));
        msgs.push_back(Message::user(input));
        return run(std::move(msgs));
    }

    std::string run(std::vector<Message> msgs) {
        return execute_loop(msgs);
    }

    // ── Parsed single-shot ────────────────────────────────────────────────

    template<output_parser Parser = TextParser>
    auto run_parsed(const std::string& input) -> typename Parser::output_type {
        auto text = run(input);
        LLMResponse synthetic{Message::assistant(text), {}, "stop", {}};
        return Parser::parse(synthetic);
    }

    // ── Multi-turn chat ───────────────────────────────────────────────────

    std::string chat(const std::string& input) {
        log_.debug(cfg_.name, "chat(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        if (history_.empty() && !cfg_.system_prompt.empty())
            history_.push_back(Message::system(cfg_.system_prompt));
        history_.push_back(Message::user(input));
        return execute_loop(history_, "chat");
    }

    // ── Tool management ───────────────────────────────────────────────────

    void add_tool(DynamicTool t) {
        log_.debug(cfg_.name, "adding tool: " + t.schema.name);
        cfg_.tools.push_back(t);
        registry_.add(std::move(t));
    }

    template<Tool T>
    void add_tool(T tool) {
        add_tool(to_dynamic_tool(std::move(tool)));
    }

    // ── Expose as a DynamicTool ───────────────────────────────────────────

    DynamicTool as_tool(std::string name, std::string description,
                        json params = json()) {
        if (params.empty())
            params = {{"type", "object"},
                      {"properties", {{"input", {{"type", "string"},
                                                  {"description", "The task to delegate"}}}}},
                      {"required", {"input"}}};

        return DynamicTool::create(
            std::move(name), std::move(description),
            [this](const json& args) -> json {
                auto input = args.contains("input")
                    ? args["input"].template get<std::string>() : args.dump();
                return json(this->run(input));
            },
            std::move(params)
        );
    }

    // ── Runnable surface ──────────────────────────────────────────────────

    std::string invoke(std::string input, const RunConfig& = {}) {
        return run(input);
    }

    std::vector<std::string> batch(std::vector<std::string> inputs, const RunConfig& cfg = {}) {
        std::vector<std::string> out;
        out.reserve(inputs.size());
        for (auto& i : inputs) out.push_back(invoke(std::move(i), cfg));
        return out;
    }

    void stream(std::string input, std::function<void(std::string)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(input), cfg));
    }

    void clear_history() { history_.clear(); }
    [[nodiscard]] const std::vector<Message>& history() const { return history_; }
    [[nodiscard]] const AgentConfig& agent_config() const { return cfg_; }
    [[nodiscard]] LLMType& llm() { return llm_; }
    [[nodiscard]] const LLMType& llm() const { return llm_; }
    [[nodiscard]] Log& log() { return log_; }
    [[nodiscard]] const Log& log() const { return log_; }
    [[nodiscard]] std::size_t tool_count() const { return registry_.size(); }
    [[nodiscard]] std::vector<ToolSchema> tool_schemas() const { return registry_.schemas(); }
};

// ─── CTAD guides ────────────────────────────────────────────────────────────

template<is_chat L>
AgentExecutor(L, AgentConfig, Log) -> AgentExecutor<deep_agent_tag, L>;

template<is_chat L>
AgentExecutor(L, AgentConfig) -> AgentExecutor<deep_agent_tag, L>;

template<is_chat L>
AgentExecutor(L) -> AgentExecutor<deep_agent_tag, L>;

// ─── Factory functions ─────────────────────────────────────────────────────

template<is_chat LLMType>
auto make_agent(LLMType llm, AgentConfig cfg = {}, Log log = {}) {
    return AgentExecutor<deep_agent_tag, LLMType>(
        std::move(llm), std::move(cfg), std::move(log));
}

template<is_chat LLMType>
auto make_shared_agent(LLMType llm, AgentConfig cfg = {}, Log log = {}) {
    return std::make_shared<AgentExecutor<deep_agent_tag, LLMType>>(
        std::move(llm), std::move(cfg), std::move(log));
}

// ─── agent_as_tool (shared_ptr ownership for nesting) ─────────────────────

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
            return json(agent->run(input));
        },
        std::move(params)
    );
}

} // namespace tiny_agent
