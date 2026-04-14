#pragma once
#include "../core/middleware.hpp"
#include <memory>
#include <atomic>
#include <unordered_map>

namespace tiny_agent::middleware {

// Limit the number of tool calls an agent may make.  Can apply globally or to
// a single named tool.  Inspired by LangChain's ToolCallLimitMiddleware.
//
//   run_limit      – max tool calls per invocation.
//   tool_name      – if set, only counts calls to this specific tool.
//   exit_behavior  – "continue" strips exceeded calls with error messages;
//                    "error" throws; "end" clears all tool calls.

struct ToolCallLimitConfig {
    int run_limit = 50;
    std::optional<std::string> tool_name;
    std::string exit_behavior = "continue";   // "continue" | "error" | "end"
};

inline MiddlewareFn tool_call_limit(ToolCallLimitConfig cfg = {}) {
    auto count = std::make_shared<std::atomic<int>>(0);

    return [cfg, count](std::vector<Message>& msgs, Next next) -> LLMResponse {
        auto resp = next(msgs);

        int matching = 0;
        for (auto& tc : resp.message.tool_calls)
            if (!cfg.tool_name || tc.name == *cfg.tool_name)
                ++matching;

        int total = count->fetch_add(matching) + matching;

        if (total > cfg.run_limit) {
            if (cfg.exit_behavior == "error")
                throw Error("Tool call limit exceeded (" +
                            std::to_string(cfg.run_limit) + ")" +
                            (cfg.tool_name ? " for tool '" + *cfg.tool_name + "'" : ""));

            if (cfg.exit_behavior == "end") {
                resp.message.tool_calls.clear();
                resp.finish_reason = "tool_call_limit";
                return resp;
            }

            // "continue" — strip only the exceeded calls, keep the rest
            if (cfg.tool_name) {
                int allowed = cfg.run_limit - (total - matching);
                std::vector<ToolCall> kept;
                int seen = 0;
                for (auto& tc : resp.message.tool_calls) {
                    if (tc.name == *cfg.tool_name) {
                        if (++seen <= allowed) kept.push_back(std::move(tc));
                    } else {
                        kept.push_back(std::move(tc));
                    }
                }
                resp.message.tool_calls = std::move(kept);
            } else {
                int allowed = cfg.run_limit - (total - matching);
                if (allowed < 0) allowed = 0;
                resp.message.tool_calls.resize(
                    static_cast<std::size_t>(std::min(
                        static_cast<int>(resp.message.tool_calls.size()),
                        allowed)));
            }
        }
        return resp;
    };
}

} // namespace tiny_agent::middleware
