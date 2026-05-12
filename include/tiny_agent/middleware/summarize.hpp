#pragma once
#include "../core/middleware.hpp"
#include "../core/model.hpp"
#include <functional>
#include <sstream>

namespace tiny_agent::middleware {

using SummarizerFn = std::function<std::string(const std::vector<Message>&)>;

inline const char* DEFAULT_SUMMARIZE_PROMPT = R"(
You are a conversation summarizer. Condense the following conversation into a concise summary that preserves:

1. The key facts, decisions, and conclusions reached
2. The overall flow of the conversation
3. Any unresolved questions or action items

Write in third person, past tense. Keep the summary under 200 words.

Conversation:
)";

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

inline std::size_t approx_tokens(const std::vector<Message>& msgs) {
    std::size_t t = 0;
    for (auto& m : msgs) t += m.text().size() / 4;
    return t;
}

inline bool compress_messages(std::vector<Message>& msgs,
                               std::size_t trigger_tokens,
                               std::size_t keep_recent,
                               const SummarizerFn& summarizer) {
    if (approx_tokens(msgs) <= trigger_tokens || msgs.size() < keep_recent + 2)
        return false;

    bool has_sys = !msgs.empty() && msgs.front().role == Role::system;
    std::size_t start = has_sys ? 1 : 0;
    std::size_t end   = msgs.size() - keep_recent;
    if (end <= start) return false;

    std::vector<Message> to_summarize(
        msgs.begin() + static_cast<long>(start),
        msgs.begin() + static_cast<long>(end));
    auto summary_text = summarizer(to_summarize);

    std::vector<Message> compressed;
    compressed.reserve(keep_recent + 2);
    if (has_sys) compressed.push_back(std::move(msgs[0]));
    compressed.push_back(
        Message::system("[Conversation summary]\n" + std::move(summary_text)));
    for (std::size_t i = end; i < msgs.size(); ++i)
        compressed.push_back(std::move(msgs[i]));
    msgs = std::move(compressed);
    return true;
}

inline std::vector<Message> chunk_and_summarize(
    const std::vector<Message>& msgs,
    std::size_t chunk_tokens,
    const std::string& prompt,
    std::function<std::string(const std::string&)> llm_call)
{
    std::vector<Message> result;
    if (msgs.empty()) return result;

    std::size_t total_tokens = approx_tokens(msgs);
    if (total_tokens <= chunk_tokens) {
        std::ostringstream ss;
        ss << prompt << extractive_summarize(msgs);
        auto summary = llm_call(ss.str());
        result.push_back(Message::assistant(summary));
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
    for (auto& s : chunk_summaries) {
        combined += "- ";
        combined += s;
        combined += '\n';
    }

    // Second pass: summarize the chunk summaries
    {
        std::ostringstream ss;
        ss << prompt << "\n\nChunk summaries:\n" << combined;
        auto final_summary = llm_call(ss.str());
        result.push_back(Message::assistant(final_summary));
    }

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
};

inline MiddlewareFn summarize(SummarizeConfig cfg = {}) {
    if (!cfg.fallback)
        cfg.fallback = [](const std::vector<Message>& m) {
            return extractive_summarize(m);
        };

    return [cfg](std::vector<Message>& msgs, Next next) -> LLMResponse {
        if (detail::approx_tokens(msgs) <= cfg.trigger_tokens || msgs.size() < cfg.keep_recent + 2)
            return next(msgs);

        bool has_sys = !msgs.empty() && msgs.front().role == Role::system;
        std::size_t start = has_sys ? 1 : 0;
        std::size_t end   = msgs.size() - cfg.keep_recent;
        if (end <= start) return next(msgs);

        std::vector<Message> to_summarize(
            msgs.begin() + static_cast<long>(start),
            msgs.begin() + static_cast<long>(end));

        std::string summary_text;
        if (cfg.llm_fn) {
            auto llm_results = detail::chunk_and_summarize(
                to_summarize, cfg.chunk_tokens, cfg.prompt, cfg.llm_fn);
            if (!llm_results.empty()) {
                summary_text = "[LLM-summarized]\n";
                for (auto& m : llm_results) summary_text += m.text() + "\n";
            }
        }

        if (summary_text.empty())
            summary_text = cfg.fallback(to_summarize);

        std::vector<Message> compressed;
        compressed.reserve(cfg.keep_recent + 2);
        if (has_sys) compressed.push_back(std::move(msgs[0]));
        compressed.push_back(
            Message::system("[Conversation summary]\n" + std::move(summary_text)));
        for (std::size_t i = end; i < msgs.size(); ++i)
            compressed.push_back(std::move(msgs[i]));
        msgs = std::move(compressed);
        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
