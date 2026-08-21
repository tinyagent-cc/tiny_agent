#pragma once
#include "base.hpp"
#include "../core/parser.hpp"
#include <memory>

namespace tiny_agent::agents {

// DeepAgent — the ReAct loop: call the model, dispatch the tools it asked for,
// feed the results back, repeat until it stops asking.
//
// Threading: one agent instance is single-threaded. run/chat/stream mutate the
// message vector and, for chat(), the retained history; two threads sharing one
// agent race. Give each thread its own agent. Sub-agents built with as_tool()
// are shared, so the same rule applies to them: a parallel tool dispatcher would
// need one sub-agent per worker.
//
// as_tool() requires the agent to be owned by a shared_ptr — build it with
// make_shared_agent() or create_agent<...>::create_shared(). Calling it on a
// stack-allocated agent throws Error rather than std::bad_weak_ptr.
template<is_chat LLMType>
class DeepAgent : public std::enable_shared_from_this<DeepAgent<LLMType>> {
    LLMType     llm_;
    AgentConfig cfg_;
    ToolRegistry    registry_;
    MiddlewareChain chain_;
    std::vector<Message> history_;

    Log&       log_()       { return cfg_.logger; }
    const Log& log_() const { return cfg_.logger; }

    LLMResponse call_llm(std::vector<Message>& msgs, const LLMConfig& overrides = {}) {
        auto schemas = registry_.schemas();
        log_().trace(cfg_.name, "sending " + std::to_string(msgs.size())
            + " messages with " + std::to_string(schemas.size()) + " tool schemas");
        auto effective = LLMConfig::merge(cfg_.llm_config, overrides);
        auto terminal = [this, &schemas](std::vector<Message>& m) -> LLMResponse {
            return llm_.chat(m, schemas);
        };
        if (chain_.empty()) return terminal(msgs);
        log_().trace(cfg_.name, "running through middleware chain");
        return chain_.run(msgs, terminal);
    }

    // Streaming counterpart of call_llm: streams the model call when the model
    // satisfies is_streaming_chat, otherwise falls back to a single chat() and
    // replays the final text as one text_delta so the handler still sees output.
    // Middleware still wraps the call in both cases.
    LLMResponse call_llm_stream(std::vector<Message>& msgs, const StreamHandler& on_event) {
        auto schemas = registry_.schemas();
        log_().trace(cfg_.name, "streaming " + std::to_string(msgs.size())
            + " messages with " + std::to_string(schemas.size()) + " tool schemas");
        auto terminal = [this, &schemas, &on_event](std::vector<Message>& m) -> LLMResponse {
            if constexpr (is_streaming_chat<LLMType>) {
                return llm_.chat_stream(m, schemas, on_event);
            } else {
                // Fallback: one chat() call, replayed as text_delta + finish so
                // the handler sees the same "deltas then finish" shape per turn.
                auto resp = llm_.chat(m, schemas);
                if (!resp.message.text().empty()) {
                    StreamEvent s{StreamEvent::Kind::text_delta};
                    s.text = resp.message.text();
                    on_event(s);
                }
                on_event(StreamEvent{StreamEvent::Kind::finish, "", -1, "", "",
                                     resp.finish_reason});
                return resp;
            }
        };
        if (chain_.empty()) return terminal(msgs);
        log_().trace(cfg_.name, "running through middleware chain");
        return chain_.run(msgs, terminal);
    }

    Message tool_failure(const ToolCall& tc, const std::string& what) {
        log_().error(cfg_.name, "tool error [" + tc.name + "]: " + what);
        json err;
        err["error"] = what;
        auto msg = Message::tool_result(tc.id, err.dump());
        msg.name = tc.name;
        return msg;
    }

    std::vector<Message> execute_tools(const std::vector<ToolCall>& calls) {
        std::vector<Message> results;
        for (auto& tc : calls) {
            log_().info(cfg_.name, "calling tool: " + tc.name);
            log_().trace(cfg_.name, "tool args [" + tc.name + "]: " + tc.arguments.dump());
            try {
                auto result = registry_.execute(tc.name, tc.arguments);
                std::string body = result.is_string()
                    ? result.template get<std::string>() : result.dump();
                log_().trace(cfg_.name, "tool result [" + tc.name + "]: " + body);
                auto msg = Message::tool_result(tc.id, std::move(body));
                msg.name = tc.name;
                results.push_back(std::move(msg));
            } catch (const std::exception& e) {
                results.push_back(tool_failure(tc, e.what()));
            } catch (...) {
                // A tool handler is user code and may throw anything. One bad
                // tool must not take down the whole agent loop: turn it into a
                // tool_result the model can read and recover from.
                results.push_back(tool_failure(tc, "unknown non-standard exception"));
            }
        }
        return results;
    }

public:
    using input_t  = std::string;
    using output_t = std::string;

    DeepAgent(LLMType llm, AgentConfig cfg = {})
        : llm_(std::move(llm)), cfg_(std::move(cfg))
    {
        if (cfg_.max_iterations <= 0)
            throw Error("AgentConfig::max_iterations must be positive (got "
                + std::to_string(cfg_.max_iterations) + ")");

        log_().debug(cfg_.name, "initializing (max_iterations=" + std::to_string(cfg_.max_iterations)
            + " tools=" + std::to_string(cfg_.tools.size())
            + " middlewares=" + std::to_string(cfg_.middlewares.size()) + ")");
        for (auto& t : cfg_.tools) {
            log_().trace(cfg_.name, "registering tool: " + t.schema.name);
            if (registry_.has(t.schema.name))
                log_().warn(cfg_.name, "duplicate tool name '" + t.schema.name
                    + "'; the later registration wins");
            registry_.add(t);
        }
        for (auto& m : cfg_.middlewares) {
            if (!m)
                throw Error("AgentConfig::middlewares contains an empty MiddlewareFn");
            chain_.add(m);
        }
    }

    std::string execute_loop(std::vector<Message>& msgs, const LLMConfig& overrides,
                             const char* label = "run") {
        for (int i = 0; i < cfg_.max_iterations; ++i) {
            log_().debug(cfg_.name, std::string(label) + " iteration " + std::to_string(i + 1)
                + "/" + std::to_string(cfg_.max_iterations)
                + " (messages=" + std::to_string(msgs.size()) + ")");
            auto resp = call_llm(msgs, overrides);
            msgs.push_back(resp.message);

            if (!resp.message.has_tool_calls()) {
                log_().debug(cfg_.name, "done: " + resp.finish_reason);
                return resp.message.text();
            }

            log_().debug(cfg_.name, "LLM requested " + std::to_string(resp.message.tool_calls.size()) + " tool call(s)");
            auto tool_results = execute_tools(resp.message.tool_calls);
            for (auto& tr : tool_results)
                msgs.push_back(std::move(tr));
        }
        log_().warn(cfg_.name, "reached max iterations (" + std::to_string(cfg_.max_iterations) + ")");
        return "Error: agent reached maximum iterations ("
            + std::to_string(cfg_.max_iterations) + ")";
    }

    // ── Streaming core loop ───────────────────────────────────────────────
    //
    // Identical control flow to execute_loop, but each model call streams:
    // text deltas and tool-call deltas flow to on_event live across every
    // iteration.  Each streamed call still returns a full LLMResponse, so the
    // loop logic (tool dispatch, termination) is unchanged.
    std::string execute_loop_stream(std::vector<Message>& msgs,
                                    const StreamHandler& on_event,
                                    const char* label = "run_stream") {
        for (int i = 0; i < cfg_.max_iterations; ++i) {
            log_().debug(cfg_.name, std::string(label) + " iteration " + std::to_string(i + 1)
                + "/" + std::to_string(cfg_.max_iterations)
                + " (messages=" + std::to_string(msgs.size()) + ")");
            auto resp = call_llm_stream(msgs, on_event);
            msgs.push_back(resp.message);

            if (!resp.message.has_tool_calls()) {
                log_().debug(cfg_.name, "done: " + resp.finish_reason);
                return resp.message.text();
            }

            log_().debug(cfg_.name, "LLM requested " + std::to_string(resp.message.tool_calls.size()) + " tool call(s)");
            auto tool_results = execute_tools(resp.message.tool_calls);
            for (auto& tr : tool_results)
                msgs.push_back(std::move(tr));
        }
        log_().warn(cfg_.name, std::string(label) + " reached max iterations ("
            + std::to_string(cfg_.max_iterations) + ")");
        return "Error: agent reached maximum iterations ("
            + std::to_string(cfg_.max_iterations) + ")";
    }

    std::string invoke(const std::string& input, const LLMConfig& overrides = {}) {
        return invoke(input, RunConfig{}, overrides);
    }

    std::string invoke(const std::string& input, const RunConfig&, const LLMConfig& overrides = {}) {
        log_().debug(cfg_.name, "invoke(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        std::vector<Message> msgs;
        if (!cfg_.system_prompt.empty())
            msgs.push_back(Message::system(cfg_.system_prompt));
        msgs.push_back(Message::user(input));
        return execute_loop(msgs, overrides);
    }

    // ── Streaming single-shot ─────────────────────────────────────────────
    //
    // Runs the full ReAct loop, streaming every model turn's deltas to
    // on_event, and returns the final text (identical to invoke()).
    std::string run_stream(const std::string& input, const StreamHandler& on_event) {
        log_().debug(cfg_.name, "run_stream(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        std::vector<Message> msgs;
        if (!cfg_.system_prompt.empty())
            msgs.push_back(Message::system(cfg_.system_prompt));
        msgs.push_back(Message::user(input));
        return execute_loop_stream(msgs, on_event, "run_stream");
    }

    std::string chat(const std::string& input, const LLMConfig& overrides = {}) {
        log_().debug(cfg_.name, "chat(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        if (history_.empty() && !cfg_.system_prompt.empty())
            history_.push_back(Message::system(cfg_.system_prompt));
        history_.push_back(Message::user(input));
        return execute_loop(history_, overrides, "chat");
    }

    // ── Streaming multi-turn chat ─────────────────────────────────────────
    std::string chat_stream(const std::string& input, const StreamHandler& on_event) {
        log_().debug(cfg_.name, "chat_stream(\"" + input.substr(0, 120)
            + (input.size() > 120 ? "..." : "") + "\")");
        if (history_.empty() && !cfg_.system_prompt.empty())
            history_.push_back(Message::system(cfg_.system_prompt));
        history_.push_back(Message::user(input));
        return execute_loop_stream(history_, on_event, "chat_stream");
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
        log_().debug(cfg_.name, "adding tool: " + t.schema.name);
        if (t.schema.name.empty())
            throw ToolError("cannot register a tool with an empty name");
        if (!t.fn)
            throw ToolError("tool '" + t.schema.name + "' has no handler");
        if (registry_.has(t.schema.name))
            log_().warn(cfg_.name, "replacing existing tool '" + t.schema.name + "'");
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
        // as_tool hands the sub-agent's lifetime to whoever holds the returned
        // tool, so the agent has to be shared-owned. Without this check a
        // stack-allocated agent throws std::bad_weak_ptr from deep inside the
        // standard library, which says nothing about how to fix it.
        if (this->weak_from_this().expired())
            throw Error("DeepAgent::as_tool() requires a shared_ptr-owned agent; "
                        "build it with make_shared_agent() or "
                        "create_agent<deep_agent_tag>::create_shared()");
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
        run_stream(input, [&](const StreamEvent& e) {
            if (e.kind == StreamEvent::Kind::text_delta) cb(e.text);
        });
    }

    void clear_history() { history_.clear(); }
    const std::vector<Message>& history() const { return history_; }
    const AgentConfig& agent_config() const { return cfg_; }
    LLMType& llm() { return llm_; }
    const LLMType& llm() const { return llm_; }
    Log& log() { return log_(); }
    const Log& log() const { return log_(); }
    std::size_t tool_count() const { return registry_.size(); }
    std::vector<ToolSchema> tool_schemas() const { return registry_.schemas(); }
};

template<is_chat L>
DeepAgent(L, AgentConfig) -> DeepAgent<L>;

template<is_chat L>
DeepAgent(L) -> DeepAgent<L>;

} // namespace tiny_agent::agents
