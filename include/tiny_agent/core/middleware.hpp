#pragma once
#include "types.hpp"
#include "log.hpp"
#include <tuple>

namespace tiny_agent {

// ── Runtime middleware (std::function-based, for user-defined lambdas) ───────

using Next = std::function<LLMResponse(std::vector<Message>&)>;

using MiddlewareFn = std::function<LLMResponse(
    std::vector<Message>&, Next)>;

class MiddlewareChain {
    std::vector<MiddlewareFn> stack_;
public:
    void add(MiddlewareFn fn) { stack_.push_back(std::move(fn)); }

    LLMResponse run(std::vector<Message>& msgs, Next terminal) const {
        Next chain = std::move(terminal);
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            auto& mw = *it;
            chain = [&mw, next = std::move(chain)](std::vector<Message>& m) {
                return mw(m, next);
            };
        }
        return chain(msgs);
    }

    bool empty() const { return stack_.empty(); }
    std::size_t size() const { return stack_.size(); }
};

// ── Concept: static middleware ───────────────────────────────────────────────

template<typename T>
concept middleware_like = requires(T mw, std::vector<Message>& msgs, Next next) {
    { mw(msgs, next) } -> std::same_as<LLMResponse>;
};

// ── Compile-time middleware stack (zero-overhead, no std::function) ──────────

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

// Built-in middleware — individual headers under middleware/ ──────────────────
#include "../middleware/all.hpp"
