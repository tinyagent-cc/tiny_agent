#pragma once
// Blanks older tool output once the conversation gets long, keeping the messages
// themselves so tool-call pairing stays intact. This is stage one of the ladder
// in context_management(); use that instead when you want the full story rather
// than this one technique.
//
// Modelled on LangChain's ContextEditingMiddleware with ClearToolUsesEdit.

#include "context.hpp"

namespace tiny_agent::middleware {

struct ContextEditingConfig {
    // Token count at which clearing kicks in.
    std::size_t trigger           = 100'000;
    // How many of the most recent tool results to leave alone.
    int         keep              = 3;
    std::string placeholder       = "[cleared]";
    // Also blank the arguments on the assistant tool calls whose results went.
    bool        clear_tool_inputs = false;
    // Empty means approx_token_count.
    TokenCounter count;
};

inline MiddlewareFn context_editing(ContextEditingConfig cfg = {}) {
    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        auto tokens = cfg.count ? cfg.count(msgs) : approx_token_count(msgs);
        if (tokens >= cfg.trigger) {
            auto keep = cfg.keep < 0 ? std::size_t{0} : static_cast<std::size_t>(cfg.keep);
            context::clear_tool_results(msgs, keep, cfg.placeholder, cfg.clear_tool_inputs);
        }
        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
