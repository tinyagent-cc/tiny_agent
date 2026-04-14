#pragma once
#include "../core/middleware.hpp"

namespace tiny_agent::middleware {

// Manage conversation context by clearing older tool-call outputs when token
// limits are approached.  Inspired by LangChain's ContextEditingMiddleware +
// ClearToolUsesEdit.
//
//   trigger            – approximate token count that triggers clearing.
//   keep               – number of most-recent tool results to preserve.
//   placeholder        – text inserted for cleared tool outputs.
//   clear_tool_inputs  – also blank tool-call arguments on assistant messages.

struct ContextEditingConfig {
    std::size_t trigger           = 100'000;
    int         keep              = 3;
    std::string placeholder       = "[cleared]";
    bool        clear_tool_inputs = false;
};

inline MiddlewareFn context_editing(ContextEditingConfig cfg = {}) {
    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        // Approximate token count (≈ 4 chars per token).
        std::size_t approx_tokens = 0;
        for (auto& m : msgs) approx_tokens += m.text().size() / 4;

        if (approx_tokens >= cfg.trigger) {
            // Count tool-result messages from the end.
            int tool_count = 0;
            for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
                if (it->role == Role::tool) ++tool_count;

            if (tool_count > cfg.keep) {
                int to_clear = tool_count - cfg.keep;
                // Walk forward (oldest first) and clear.
                for (auto& m : msgs) {
                    if (to_clear <= 0) break;
                    if (m.role == Role::tool) {
                        m.content = cfg.placeholder;
                        --to_clear;
                    }
                }
            }

            if (cfg.clear_tool_inputs) {
                // Walk assistant messages and strip tool-call arguments
                // for tool calls whose results were cleared.
                int remaining = tool_count - cfg.keep;
                for (auto& m : msgs) {
                    if (remaining <= 0) break;
                    if (m.role == Role::assistant) {
                        for (auto& tc : m.tool_calls) {
                            if (remaining <= 0) break;
                            tc.arguments = json::object();
                            --remaining;
                        }
                    }
                }
            }
        }

        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
