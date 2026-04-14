#pragma once
#include "core/types.hpp"
#include "core/tool.hpp"
#include "core/llm.hpp"
#include "core/middleware.hpp"
#include "core/log.hpp"
#include "core/parser.hpp"
#include <memory>

namespace tiny_agent {

// ── Agent configuration (everything except the LLM and logger) ──────────────

struct AgentConfig {
    std::string name = "agent";
    std::string system_prompt;
    std::vector<Tool>          tools;
    std::vector<MiddlewareFn>  middlewares;
    int max_iterations = 10;
};

// ── Agent<LLMType> — fully static-dispatch agent loop ────────────────────────
//
// LLMType must satisfy llm_like (e.g. LLM<openai>, LLM<anthropic>, AnyLLM).
//
// Usage:
//   auto agent = Agent{LLM<openai>{"gpt-4o", key}, config};
//   auto result = agent.run("What is 2+2?");
//
// Logging (default level: warn — set lower to see agent internals):
//   auto agent = Agent{LLM<openai>{"gpt-4o", key}, config,
//                      Log{std::cerr, LogLevel::debug}};

template<llm_like LLMType>
class Agent : public std::enable_shared_from_this<Agent<LLMType>> {
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
    Agent(LLMType llm, AgentConfig cfg = {}, Log log = {})
        : cfg_(std::move(cfg)), llm_(std::move(llm)), log_(std::move(log))
    {
        log_.debug(cfg_.name, "initializing (max_iterations=" + std::to_string(cfg_.max_iterations)
            + " tools=" + std::to_string(cfg_.tools.size())
            + " middlewares=" + std::to_string(cfg_.middlewares.size()) + ")");
        if (!cfg_.system_prompt.empty())
            log_.trace(cfg_.name, "system_prompt: " + cfg_.system_prompt);
        for (auto& t : cfg_.tools) {
            log_.trace(cfg_.name, "registering tool: " + t.schema.name);
            registry_.add(t);
        }
        for (auto& m : cfg_.middlewares) chain_.add(m);
    }

    // ── Single-shot execution ───────────────────────────────────────────

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
        for (int i = 0; i < cfg_.max_iterations; ++i) {
            log_.debug(cfg_.name, "iteration " + std::to_string(i + 1)
                + "/" + std::to_string(cfg_.max_iterations)
                + " (messages=" + std::to_string(msgs.size()) + ")");
            auto resp = call_llm(msgs);
            msgs.push_back(resp.message);

            if (!resp.message.has_tool_calls()) {
                log_.debug(cfg_.name, "done: " + resp.finish_reason);
                log_.trace(cfg_.name, "response text: " + resp.message.text().substr(0, 200));
                return resp.message.text();
            }

            log_.debug(cfg_.name, "LLM requested " + std::to_string(resp.message.tool_calls.size()) + " tool call(s)");
            auto tool_results = execute_tools(resp.message.tool_calls);
            for (auto& tr : tool_results)
                msgs.push_back(std::move(tr));
        }
        log_.warn(cfg_.name, "reached max iterations (" + std::to_string(cfg_.max_iterations) + ")");
        return msgs.back().text();
    }

    // ── Parsed single-shot (structured output) ─────────────────────────

    template<output_parser Parser = TextParser>
    auto run_parsed(const std::string& input) -> typename Parser::output_type {
        auto text = run(input);
        LLMResponse synthetic{Message::assistant(text), {}, "stop", {}};
        return Parser::parse(synthetic);
    }

    // ── Multi-turn chat ─────────────────────────────────────────────────

    std::string chat(const std::string& input) {
        log_.debug(cfg_.name, "chat(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        if (history_.empty() && !cfg_.system_prompt.empty())
            history_.push_back(Message::system(cfg_.system_prompt));
        history_.push_back(Message::user(input));

        for (int i = 0; i < cfg_.max_iterations; ++i) {
            log_.debug(cfg_.name, "chat iteration " + std::to_string(i + 1)
                + " (history=" + std::to_string(history_.size()) + ")");
            auto resp = call_llm(history_);
            history_.push_back(resp.message);

            if (!resp.message.has_tool_calls())
                return resp.message.text();

            log_.debug(cfg_.name, "LLM requested " + std::to_string(resp.message.tool_calls.size()) + " tool call(s)");
            auto tool_results = execute_tools(resp.message.tool_calls);
            for (auto& tr : tool_results)
                history_.push_back(std::move(tr));
        }
        log_.warn(cfg_.name, "chat reached max iterations (" + std::to_string(cfg_.max_iterations) + ")");
        return history_.back().text();
    }

    // ── Tool management ─────────────────────────────────────────────────

    void add_tool(Tool t) {
        log_.debug(cfg_.name, "adding tool: " + t.schema.name);
        cfg_.tools.push_back(t);
        registry_.add(std::move(t));
    }

    // ── Expose this agent as a tool (captures this — ensure lifetime) ───

    Tool as_tool(std::string name, std::string description,
                 json params = json()) {
        if (params.empty())
            params = {{"type", "object"},
                      {"properties", {{"input", {{"type", "string"},
                                                  {"description", "The task to delegate"}}}}},
                      {"required", {"input"}}};

        return Tool::create(
            std::move(name), std::move(description),
            [this](const json& args) -> json {
                auto input = args.contains("input")
                    ? args["input"].template get<std::string>() : args.dump();
                return json(this->run(input));
            },
            std::move(params)
        );
    }

    void clear_history() { history_.clear(); }
    [[nodiscard]] const std::vector<Message>& history() const { return history_; }
    [[nodiscard]] const AgentConfig& config() const { return cfg_; }
    [[nodiscard]] LLMType& llm() { return llm_; }
    [[nodiscard]] const LLMType& llm() const { return llm_; }
    [[nodiscard]] Log& log() { return log_; }
    [[nodiscard]] const Log& log() const { return log_; }
};

// ── CTAD guides ─────────────────────────────────────────────────────────────

template<llm_like L>
Agent(L, AgentConfig, Log) -> Agent<L>;

template<llm_like L>
Agent(L, AgentConfig) -> Agent<L>;

template<llm_like L>
Agent(L) -> Agent<L>;

// ── Shared agent factories (for as_tool with shared ownership) ──────────────

template<llm_like LLMType>
auto make_shared_agent(LLMType llm, AgentConfig cfg = {}, Log log = {}) {
    return std::make_shared<Agent<LLMType>>(
        std::move(llm), std::move(cfg), std::move(log));
}

// ── Free function: agent_as_tool (shared_ptr ownership for nesting) ─────────

template<typename AgentType>
Tool agent_as_tool(std::shared_ptr<AgentType> agent,
                   std::string name, std::string description,
                   json params = json()) {
    if (params.empty())
        params = {{"type", "object"},
                  {"properties", {{"input", {{"type", "string"},
                                              {"description", "The task to delegate"}}}}},
                  {"required", {"input"}}};

    return Tool::create(
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
