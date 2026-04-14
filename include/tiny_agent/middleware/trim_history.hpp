#pragma once
#include "../core/middleware.hpp"

namespace tiny_agent::middleware {

template<std::size_t MaxMessages = 50>
struct TrimHistory {
    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        if (msgs.size() > MaxMessages + 1) {
            bool has_sys = !msgs.empty() && msgs.front().role == Role::system;
            std::size_t start = has_sys ? 1 : 0;
            auto excess = msgs.size() - (has_sys ? 1 : 0) - MaxMessages;
            msgs.erase(msgs.begin() + static_cast<long>(start),
                       msgs.begin() + static_cast<long>(start + excess));
        }
        return next(msgs);
    }
};

inline MiddlewareFn trim_history(std::size_t max_messages) {
    return [=](std::vector<Message>& msgs, Next next) -> LLMResponse {
        if (msgs.size() > max_messages + 1) {
            bool has_sys = !msgs.empty() && msgs.front().role == Role::system;
            std::size_t start = has_sys ? 1 : 0;
            auto excess = msgs.size() - (has_sys ? 1 : 0) - max_messages;
            msgs.erase(msgs.begin() + static_cast<long>(start),
                       msgs.begin() + static_cast<long>(start + excess));
        }
        return next(msgs);
    };
}

static_assert(middleware_like<TrimHistory<>>);

} // namespace tiny_agent::middleware
