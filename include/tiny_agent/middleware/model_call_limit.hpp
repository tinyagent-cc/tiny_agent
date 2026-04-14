#pragma once
#include "../core/middleware.hpp"
#include <memory>
#include <atomic>

namespace tiny_agent::middleware {

// Limit the number of model (LLM) calls to prevent runaway loops or excessive
// API spend.  Inspired by LangChain's ModelCallLimitMiddleware.
//
//   run_limit   – max calls per single agent.run() / agent.chat() invocation.
//   exit_behavior – "end" returns an error AIMessage; "error" throws.

struct ModelCallLimitConfig {
    int run_limit = 25;
    std::string exit_behavior = "end";   // "end" | "error"
};

inline MiddlewareFn model_call_limit(ModelCallLimitConfig cfg = {}) {
    auto count = std::make_shared<std::atomic<int>>(0);

    return [cfg, count](std::vector<Message>& msgs, Next next) -> LLMResponse {
        int n = count->fetch_add(1) + 1;
        if (n > cfg.run_limit) {
            if (cfg.exit_behavior == "error")
                throw Error("Model call limit exceeded (" +
                            std::to_string(cfg.run_limit) + ")");
            return {Message::assistant(
                        "I've reached the maximum number of reasoning steps (" +
                        std::to_string(cfg.run_limit) + "). Here is my best answer so far."),
                    {}, "model_call_limit", {}};
        }
        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
