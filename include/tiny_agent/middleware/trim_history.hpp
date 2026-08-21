#pragma once
// Keeps the conversation under a fixed *message count*. For a token budget and
// the gentler techniques that come before dropping messages, use
// context_management() in context.hpp — this is the blunt instrument.
//
// Trimming snaps past orphaned tool results, so the surviving conversation never
// starts with a tool message the provider would reject.

#include "context.hpp"

namespace tiny_agent::middleware {

template<std::size_t MaxMessages = 50>
struct TrimHistory {
    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        context::trim_to_message_count(msgs, MaxMessages);
        return next(msgs);
    }
};

inline MiddlewareFn trim_history(std::size_t max_messages) {
    return [=](std::vector<Message>& msgs, Next next) -> LLMResponse {
        context::trim_to_message_count(msgs, max_messages);
        return next(msgs);
    };
}

static_assert(middleware_like<TrimHistory<>>);

} // namespace tiny_agent::middleware
