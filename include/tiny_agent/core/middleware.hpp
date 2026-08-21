#pragma once
#include "types.hpp"
#include "log.hpp"
#include <tuple>

namespace tiny_agent {

using Next = std::function<LLMResponse(std::vector<Message>&)>;

using MiddlewareFn = std::function<LLMResponse(
    std::vector<Message>&, Next)>;

// MiddlewareChain — the runtime chain. Each middleware receives the mutable
// message vector and a Next it must call to reach the model.
//
// A middleware may: inspect messages, rewrite them in place, short-circuit by
// returning its own LLMResponse without calling next, or call next more than
// once (retry). Anything it throws propagates to the caller with the message
// vector left in whatever state it had reached — a middleware that needs the
// original messages back on failure has to save and restore them itself.
//
// Threading: build the chain, then run it. run() holds pointers into the
// internal vector for the duration of the call, so calling add() from another
// thread while run() is in flight is a use-after-free. The chain is safe to run
// concurrently as long as nothing mutates it.
class MiddlewareChain {
    std::vector<MiddlewareFn> stack_;
public:
    void add(MiddlewareFn fn) {
        if (!fn) throw Error("MiddlewareChain::add: empty MiddlewareFn");
        stack_.push_back(std::move(fn));
    }

    LLMResponse run(std::vector<Message>& msgs, Next terminal) const {
        if (!terminal) throw Error("MiddlewareChain::run: empty terminal");
        Next chain = std::move(terminal);
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            auto& mw = *it;
            chain = [&mw, next = std::move(chain)](std::vector<Message>& m) {
                return mw(m, next);
            };
        }
        return chain(msgs);
    }

    void clear() { stack_.clear(); }
    bool empty() const { return stack_.empty(); }
    std::size_t size() const { return stack_.size(); }
};

template<typename T>
concept middleware_like = requires(T mw, std::vector<Message>& msgs, Next next) {
    { mw(msgs, next) } -> std::same_as<LLMResponse>;
};

template<middleware_like... Mws>
class StaticMiddlewareStack {
    std::tuple<Mws...> mws_;

    template<std::size_t I, typename Terminal>
    LLMResponse apply(std::vector<Message>& msgs, Terminal&& terminal) const {
        if constexpr (I == sizeof...(Mws)) {
            return terminal(msgs);
        } else {
            return std::get<I>(mws_)(msgs,
                [this, &terminal](std::vector<Message>& m) -> LLMResponse {
                    return apply<I + 1>(m, terminal);
                });
        }
    }

public:
    explicit StaticMiddlewareStack(Mws... mws) : mws_(std::move(mws)...) {}

    template<typename Terminal>
    LLMResponse run(std::vector<Message>& msgs, Terminal&& terminal) const {
        return apply<0>(msgs, std::forward<Terminal>(terminal));
    }

    static constexpr bool empty() { return sizeof...(Mws) == 0; }
};

template<middleware_like... Mws>
auto make_middleware_stack(Mws&&... mws) {
    return StaticMiddlewareStack<std::remove_cvref_t<Mws>...>(
        std::forward<Mws>(mws)...);
}

} // namespace tiny_agent
