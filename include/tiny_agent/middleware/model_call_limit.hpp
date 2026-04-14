#pragma once
#include "../core/middleware.hpp"
#include <memory>
#include <atomic>

namespace tiny_agent::middleware {

// Limit the total number of model (LLM) calls across the middleware's
// lifetime to prevent runaway loops or excessive API spend.
//
//   limit         – max calls for this middleware instance's lifetime.
//                   For per-run limits, create a fresh middleware per invocation.
//   exit_behavior – "end" returns an error AIMessage; "error" throws.

struct ModelCallLimitConfig {
    int limit = 25;
    std::string exit_behavior = "end";   // "end" | "error"
};

inline MiddlewareFn model_call_limit(ModelCallLimitConfig cfg = {}) {
    auto count = std::make_shared<std::atomic<int>>(0);

    return [cfg, count](std::vector<Message>& msgs, Next next) -> LLMResponse {
        int n = count->fetch_add(1) + 1;
        if (n > cfg.limit) {
            if (cfg.exit_behavior == "error")
                throw Error("Model call limit exceeded (" +
                            std::to_string(cfg.limit) + ")");
            return {Message::assistant(
                        "I've reached the maximum number of reasoning steps (" +
                        std::to_string(cfg.limit) + "). Here is my best answer so far."),
                    {}, "model_call_limit", {}};
        }
        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
