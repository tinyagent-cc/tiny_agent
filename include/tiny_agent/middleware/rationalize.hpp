#pragma once
#include "../core/middleware.hpp"

namespace tiny_agent::middleware {

// ── Tool-efficiency rationalization ─────────────────────────────────────────
//
// Detects large content in the conversation (typically tool results) and
// temporarily injects efficiency guidance into the system prompt for the
// current LLM call.  Guides the model toward using targeted operations
// (sed, grep, shell pipes) instead of re-processing large payloads inline.
//
// Guidance is injected only for the LLM call and removed afterwards — it
// does not accumulate across turns.
//
// Research context: SkillReducer (2026) showed 48 % description compression
// while improving quality; Ares (2026) demonstrated 52.7 % token savings
// through adaptive effort selection.

struct RationalizeConfig {
    std::size_t              large_threshold = 2000;  // approx tokens
    std::vector<std::string> hints;                   // efficiency hints
};

namespace detail {

inline std::vector<std::string> default_rationalize_hints() {
    return {
        "Prefer targeted shell commands (sed, awk, grep) over inline processing of large content",
        "Use precise file operations (write specific lines) rather than rewriting entire files",
        "Combine multiple small operations into a single command when possible",
        "Narrow searches with specific patterns rather than scanning broad results",
    };
}

inline std::string build_guidance(const std::vector<Message>& msgs,
                                  std::size_t threshold,
                                  const std::vector<std::string>& hints)
{
    std::string guidance;
    int large_count = 0;
    std::size_t max_size = 0;

    for (auto& m : msgs) {
        if (m.role == Role::tool) {
            auto sz = m.text().size() / 4;
            if (sz > threshold) {
                ++large_count;
                if (sz > max_size) max_size = sz;
            }
        }
    }

    if (large_count == 0) return {};

    guidance = "\n\n[Efficiency guidance — " +
               std::to_string(large_count) + " large result(s), ~" +
               std::to_string(max_size) + " tokens max]\n";
    for (auto& h : hints)
        { guidance += "- "; guidance += h; guidance += '\n'; }
    return guidance;
}

}  // namespace detail

// ── Compile-time middleware ─────────────────────────────────────────────────

template<std::size_t LargeThreshold = 2000>
struct Rationalize {
    std::vector<std::string> hints;

    Rationalize() : hints(detail::default_rationalize_hints()) {}
    explicit Rationalize(std::vector<std::string> h) : hints(std::move(h)) {}

    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        auto guidance = detail::build_guidance(msgs, LargeThreshold, hints);
        if (guidance.empty()) return next(msgs);

        if (!msgs.empty() && msgs.front().role == Role::system) {
            auto original = msgs.front().text();
            msgs.front().content = original + guidance;
            auto resp = next(msgs);
            msgs.front().content = std::move(original);
            return resp;
        }

        msgs.insert(msgs.begin(), Message::system(guidance));
        auto resp = next(msgs);
        msgs.erase(msgs.begin());
        return resp;
    }
};

// ── Runtime middleware ──────────────────────────────────────────────────────

inline MiddlewareFn rationalize(RationalizeConfig cfg = {}) {
    if (cfg.hints.empty())
        cfg.hints = detail::default_rationalize_hints();

    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        auto guidance = detail::build_guidance(
            msgs, cfg.large_threshold, cfg.hints);
        if (guidance.empty()) return next(msgs);

        if (!msgs.empty() && msgs.front().role == Role::system) {
            auto original = msgs.front().text();
            msgs.front().content = original + guidance;
            auto resp = next(msgs);
            msgs.front().content = std::move(original);
            return resp;
        }

        msgs.insert(msgs.begin(), Message::system(guidance));
        auto resp = next(msgs);
        msgs.erase(msgs.begin());
        return resp;
    };
}

static_assert(middleware_like<Rationalize<>>);

} // namespace tiny_agent::middleware
