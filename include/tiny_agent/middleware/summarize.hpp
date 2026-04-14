#pragma once
#include "../core/middleware.hpp"
#include <functional>

namespace tiny_agent::middleware {

// ── Conversation summarization ──────────────────────────────────────────────
//
// Compresses older messages into a summary when approximate token count
// exceeds a threshold.  Keeps the system prompt and the N most-recent
// messages intact; everything in between is replaced by a single summary
// message.
//
// Default summarizer is extractive (no LLM call) — safe for constrained
// hardware.  Users may supply an LLM-backed summarizer via SummarizerFn.
//
// Inspired by LangChain's ConversationSummaryMemory.

using SummarizerFn = std::function<std::string(const std::vector<Message>&)>;

// Extractive summarizer: condense each message to a role-prefixed snippet.
// Tool results are truncated aggressively (they tend to be large but low-
// value once consumed).  No LLM required.
inline std::string extractive_summarize(
    const std::vector<Message>& msgs, std::size_t max_per_msg = 150)
{
    std::string out;
    out.reserve(msgs.size() * (max_per_msg + 20));
    for (auto& m : msgs) {
        auto t = m.text();
        if (t.empty()) continue;
        std::size_t limit = (m.role == Role::tool) ? max_per_msg / 3 : max_per_msg;
        out += to_string(m.role);
        if (m.name) { out += '('; out += *m.name; out += ')'; }
        out += ": ";
        if (t.size() > limit) { out += t.substr(0, limit); out += "..."; }
        else                    out += t;
        out += '\n';
    }
    return out;
}

// ── Compile-time middleware ─────────────────────────────────────────────────

template<std::size_t TriggerTokens = 4000, std::size_t KeepRecent = 4>
struct Summarize {
    SummarizerFn summarizer;

    Summarize()
        : summarizer([](const std::vector<Message>& m) {
              return extractive_summarize(m);
          }) {}
    explicit Summarize(SummarizerFn fn) : summarizer(std::move(fn)) {}

    LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
        compress(msgs);
        return next(msgs);
    }

private:
    void compress(std::vector<Message>& msgs) const {
        std::size_t approx_tokens = 0;
        for (auto& m : msgs) approx_tokens += m.text().size() / 4;

        if (approx_tokens <= TriggerTokens || msgs.size() < KeepRecent + 2)
            return;

        bool has_sys = !msgs.empty() && msgs.front().role == Role::system;
        std::size_t start = has_sys ? 1 : 0;
        std::size_t end   = msgs.size() - KeepRecent;
        if (end <= start) return;

        std::vector<Message> to_summarize(
            msgs.begin() + static_cast<long>(start),
            msgs.begin() + static_cast<long>(end));
        auto summary_text = summarizer(to_summarize);

        std::vector<Message> compressed;
        compressed.reserve(KeepRecent + 2);
        if (has_sys) compressed.push_back(std::move(msgs[0]));
        compressed.push_back(
            Message::system("[Conversation summary]\n" + std::move(summary_text)));
        for (std::size_t i = end; i < msgs.size(); ++i)
            compressed.push_back(std::move(msgs[i]));
        msgs = std::move(compressed);
    }
};

// ── Runtime middleware ──────────────────────────────────────────────────────

struct SummarizeConfig {
    std::size_t  trigger_tokens = 4000;
    std::size_t  keep_recent    = 4;
    SummarizerFn summarizer;
};

inline MiddlewareFn summarize(SummarizeConfig cfg = {}) {
    if (!cfg.summarizer)
        cfg.summarizer = [](const std::vector<Message>& m) {
            return extractive_summarize(m);
        };

    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        std::size_t approx_tokens = 0;
        for (auto& m : msgs) approx_tokens += m.text().size() / 4;

        if (approx_tokens > cfg.trigger_tokens &&
            msgs.size() >= cfg.keep_recent + 2)
        {
            bool has_sys = !msgs.empty() && msgs.front().role == Role::system;
            std::size_t start = has_sys ? 1 : 0;
            std::size_t end   = msgs.size() - cfg.keep_recent;

            if (end > start) {
                std::vector<Message> to_summarize(
                    msgs.begin() + static_cast<long>(start),
                    msgs.begin() + static_cast<long>(end));
                auto summary_text = cfg.summarizer(to_summarize);

                std::vector<Message> compressed;
                compressed.reserve(cfg.keep_recent + 2);
                if (has_sys) compressed.push_back(std::move(msgs[0]));
                compressed.push_back(Message::system(
                    "[Conversation summary]\n" + std::move(summary_text)));
                for (std::size_t i = end; i < msgs.size(); ++i)
                    compressed.push_back(std::move(msgs[i]));
                msgs = std::move(compressed);
            }
        }
        return next(msgs);
    };
}

static_assert(middleware_like<Summarize<>>);

} // namespace tiny_agent::middleware
