#pragma once
#include "../core/middleware.hpp"
#include <chrono>
#include <thread>
#include <cmath>
#include <random>
#include <algorithm>

namespace tiny_agent::middleware {

// Enhanced model-call retry with configurable exponential backoff, jitter,
// and failure modes.  Inspired by LangChain's ModelRetryMiddleware.

struct ModelRetryConfig {
    int    max_retries     = 2;
    double backoff_factor  = 2.0;
    double initial_delay   = 1000.0;   // ms
    double max_delay       = 60000.0;  // ms
    bool   jitter          = true;     // ±25 %
    std::string on_failure = "continue";  // "continue" | "error"
};

inline MiddlewareFn model_retry(ModelRetryConfig cfg = {}) {
    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<> jitter_dist(0.75, 1.25);

        for (int attempt = 0; attempt <= cfg.max_retries; ++attempt) {
            try {
                return next(msgs);
            } catch (const APIError& e) {
                if (attempt >= cfg.max_retries) {
                    if (cfg.on_failure == "error") throw;
                    return {Message::assistant(
                                "Model call failed after " +
                                std::to_string(cfg.max_retries + 1) +
                                " attempts: " + std::string(e.what())),
                            {}, "error", {}};
                }

                double delay = cfg.initial_delay *
                    std::pow(cfg.backoff_factor, static_cast<double>(attempt));
                delay = std::min(delay, cfg.max_delay);
                if (cfg.jitter) delay *= jitter_dist(rng);

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(static_cast<long long>(delay)));
            }
        }
        throw Error("model_retry: unreachable");
    };
}

} // namespace tiny_agent::middleware
