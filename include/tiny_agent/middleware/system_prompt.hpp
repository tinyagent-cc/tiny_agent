#pragma once
#include "../core/middleware.hpp"

namespace tiny_agent::middleware {

struct SystemPrompt {
    std::string prompt;
    explicit SystemPrompt(std::string p) : prompt(std::move(p)) {}

    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        if (msgs.empty() || msgs.front().role != Role::system)
            msgs.insert(msgs.begin(), Message::system(prompt));
        return next(msgs);
    }
};

inline MiddlewareFn system_prompt(std::string prompt) {
    return SystemPrompt{std::move(prompt)};
}

static_assert(middleware_like<SystemPrompt>);

} // namespace tiny_agent::middleware
