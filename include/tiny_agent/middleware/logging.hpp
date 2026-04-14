#pragma once
#include "../core/middleware.hpp"
#include <chrono>

namespace tiny_agent::middleware {

struct Logging {
    Log log;
    explicit Logging(Log l = {LogLevel::debug}) : log(std::move(l)) {}

    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        log.debug("middleware", "sending " + std::to_string(msgs.size()) + " messages");
        for (std::size_t i = 0; i < msgs.size(); ++i)
            log.trace("middleware", "  [" + std::to_string(i) + "] role="
                + to_string(msgs[i].role) + " text=" + msgs[i].text().substr(0, 100));

        auto start = std::chrono::steady_clock::now();
        auto resp = next(msgs);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        log.debug("middleware", "response: " + resp.finish_reason
            + " (tool_calls=" + std::to_string(resp.message.tool_calls.size())
            + " elapsed=" + std::to_string(elapsed) + "ms)");
        return resp;
    }
};

inline MiddlewareFn logging(Log log = {LogLevel::debug}) {
    return Logging{std::move(log)};
}

static_assert(middleware_like<Logging>);

} // namespace tiny_agent::middleware
