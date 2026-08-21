#pragma once
// Folds the middle of a long conversation into one summary message, keeping the
// system prompt and the recent turns verbatim. This is stage two of the ladder
// in context_management(); use that instead when you want the full story rather
// than this one technique.
//
// The summary can be extractive (no model call, the default) or written by a
// model via llm_fn. Long histories are summarized in chunks and then the chunk
// summaries are summarized again, so one oversized conversation does not become
// one oversized summarization prompt.

#include "context.hpp"
#include "../core/model.hpp"
#include <functional>
#include <sstream>

namespace tiny_agent::middleware {

// SummarizerFn, extractive_summarize, TokenCounter and approx_token_count come
// from context.hpp — they are shared with the other context middleware.

inline const char* DEFAULT_SUMMARIZE_PROMPT = R"(
You are a conversation summarizer. Condense the following conversation into a concise summary that preserves:

1. The key facts, decisions, and conclusions reached
2. The overall flow of the conversation
3. Any unresolved questions or action items

Write in third person, past tense. Keep the summary under 200 words.

Conversation:
)";

struct LLMSummarizer {
    std::string prompt = DEFAULT_SUMMARIZE_PROMPT;
    std::function<std::vector<Message>(const std::vector<Message>&)> llm;

    explicit LLMSummarizer(std::function<std::vector<Message>(const std::vector<Message>&)> llm_fn,
                           std::string p = DEFAULT_SUMMARIZE_PROMPT)
        : prompt(std::move(p)), llm(std::move(llm_fn)) {}

    std::string operator()(const std::vector<Message>& msgs) const {
        auto reply = llm(msgs);
        std::string text;
        for (auto& m : reply) text += m.text();
        return text;
    }
};

namespace detail {

// Retained for callers that used it directly; approx_token_count is the one to
// reach for now, since it also counts tool-call arguments.
inline std::size_t approx_tokens(const std::vector<Message>& msgs) {
    return approx_token_count(msgs);
}

inline bool compress_messages(std::vector<Message>& msgs,
                              std::size_t trigger_tokens,
                              std::size_t keep_recent,
                              const SummarizerFn& summarizer) {
    if (approx_token_count(msgs) <= trigger_tokens || msgs.size() < keep_recent + 2)
        return false;
    return context::summarize_middle(msgs, keep_recent, summarizer,
                                     "[Conversation summary]\n");
}

// Splits an oversized history into chunks the summarizer can digest, summarizes
// each, then summarizes the summaries.
inline std::vector<Message> chunk_and_summarize(
    const std::vector<Message>& msgs,
    std::size_t chunk_tokens,
    const std::string& prompt,
    std::function<std::string(const std::string&)> llm_call)
{
    std::vector<Message> result;
    if (msgs.empty()) return result;

    std::size_t total_tokens = approx_token_count(msgs);
    if (total_tokens <= chunk_tokens) {
        std::ostringstream ss;
        ss << prompt << extractive_summarize(msgs);
        result.push_back(Message::assistant(llm_call(ss.str())));
        return result;
    }

    std::size_t n_chunks = (total_tokens + chunk_tokens - 1) / chunk_tokens;
    std::size_t chunk_size = msgs.size() / n_chunks;
    if (chunk_size < 1) chunk_size = 1;

    std::vector<std::string> chunk_summaries;
    for (std::size_t i = 0; i < msgs.size(); i += chunk_size) {
        auto end = std::min(i + chunk_size, msgs.size());
        std::vector<Message> chunk(msgs.begin() + static_cast<long>(i),
                                   msgs.begin() + static_cast<long>(end));
        std::ostringstream ss;
        ss << prompt << extractive_summarize(chunk);
        chunk_summaries.push_back(llm_call(ss.str()));
    }

    if (chunk_summaries.size() == 1) {
        result.push_back(Message::assistant(chunk_summaries[0]));
        return result;
    }

    std::string combined;
    for (auto& s : chunk_summaries) { combined += "- "; combined += s; combined += '\n'; }

    std::ostringstream ss;
    ss << prompt << "\n\nChunk summaries:\n" << combined;
    result.push_back(Message::assistant(llm_call(ss.str())));
    return result;
}

} // namespace detail

struct SummarizeConfig {
    std::size_t  trigger_tokens   = 4000;
    std::size_t  keep_recent      = 4;
    std::size_t  chunk_tokens     = 2000;
    std::string  prompt           = DEFAULT_SUMMARIZE_PROMPT;
    SummarizerFn fallback;
    std::function<std::string(const std::string&)> llm_fn;
    TokenCounter count;
};

inline MiddlewareFn summarize(SummarizeConfig cfg = {}) {
    if (!cfg.fallback)
        cfg.fallback = [](const std::vector<Message>& m) { return extractive_summarize(m); };

    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        auto tokens = cfg.count ? cfg.count(msgs) : approx_token_count(msgs);
        if (tokens <= cfg.trigger_tokens || msgs.size() < cfg.keep_recent + 2)
            return next(msgs);

        // Route the whole summarization through one summarizer so the middle is
        // sliced at a tool-safe boundary exactly once.
        SummarizerFn summarizer = [&cfg](const std::vector<Message>& middle) -> std::string {
            if (cfg.llm_fn) {
                auto parts = detail::chunk_and_summarize(middle, cfg.chunk_tokens,
                                                         cfg.prompt, cfg.llm_fn);
                if (!parts.empty()) {
                    std::string text = "[LLM-summarized]\n";
                    for (auto& m : parts) { text += m.text(); text += '\n'; }
                    return text;
                }
            }
            return cfg.fallback(middle);
        };

        context::summarize_middle(msgs, cfg.keep_recent, summarizer,
                                  "[Conversation summary]\n");
        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
