#pragma once
#include "../core/middleware.hpp"
#include <chrono>
#include <thread>

namespace tiny_agent::middleware {

template<int MaxRetries = 3, int BackoffMs = 500>
struct Retry {
    Log log;
    Retry() = default;
    explicit Retry(Log l) : log(std::move(l)) {}

    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        for (int attempt = 0; ; ++attempt) {
            try { return next(msgs); }
            catch (const APIError& e) {
                if (attempt >= MaxRetries || e.status_code < 500) throw;
                auto delay = BackoffMs * (1 << attempt);
                log.warn("retry", "attempt " + std::to_string(attempt + 1)
                    + "/" + std::to_string(MaxRetries) + " failed (status="
                    + std::to_string(e.status_code) + "), retrying in "
                    + std::to_string(delay) + "ms");
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
    }
};

inline MiddlewareFn retry(int max_retries = 3,
                          std::chrono::milliseconds backoff = std::chrono::milliseconds(500),
                          Log log = {}) {
    return [=](std::vector<Message>& msgs, Next next) -> LLMResponse {
        for (int attempt = 0; ; ++attempt) {
            try { return next(msgs); }
            catch (const APIError& e) {
                if (attempt >= max_retries || e.status_code < 500) throw;
                auto delay = backoff * (1 << attempt);
                log.warn("retry", "attempt " + std::to_string(attempt + 1)
                    + "/" + std::to_string(max_retries) + " failed (status="
                    + std::to_string(e.status_code) + "), retrying in "
                    + std::to_string(delay.count()) + "ms");
                std::this_thread::sleep_for(delay);
            }
        }
    };
}

static_assert(middleware_like<Retry<>>);

} // namespace tiny_agent::middleware
