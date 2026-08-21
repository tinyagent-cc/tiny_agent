#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  middleware/context.hpp  —  keeping the prompt inside a token budget
//
//  Three middleware used to answer the same question separately: trim_history
//  dropped old messages, summarize folded them into a summary, context_editing
//  blanked stale tool output. Each counted tokens its own way, each sliced the
//  message vector its own way, and stacking them meant reasoning about three
//  independent triggers.
//
//  This header holds the shared machinery and one middleware over it.
//  context_management() states a budget and, when the conversation exceeds it,
//  escalates through the three techniques in cost order, stopping as soon as the
//  conversation fits:
//
//    1. clear old tool results   — cheapest, loses the most bytes per message
//    2. summarize the middle     — keeps the thread, costs a summarizer call
//    3. drop the oldest          — last resort, loses information outright
//
//  The three original middleware remain, unchanged in behaviour and now built on
//  the same primitives, so an existing stack keeps working.
//
//  ── Tool-call pairing ──
//
//  Providers reject a `tool` message that does not follow the assistant message
//  requesting it. Any cut that lands mid tool-call sequence produces a prompt
//  the API refuses. Every function here that removes messages snaps its cut
//  point forward past orphaned tool results, so the surviving conversation is
//  always well formed.
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/middleware.hpp"
#include "../core/log.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>

namespace tiny_agent::middleware {

// ─── Token counting ───────────────────────────────────────────────────────────

using TokenCounter = std::function<std::size_t(const std::vector<Message>&)>;
using SummarizerFn = std::function<std::string(const std::vector<Message>&)>;

// Rough count with no tokenizer: about four characters per token, plus a few
// tokens of per-message role and delimiter overhead.
//
// It counts tool-call arguments as well as message text. Text alone misses them
// entirely, so a tool-heavy conversation — exactly the kind that overruns a
// context window — reads as far smaller than it is.
//
// Swap in a real tokenizer through TokenBudget::count when you need precision.
inline std::size_t approx_token_count(const std::vector<Message>& msgs) {
    constexpr std::size_t kCharsPerToken = 4;
    constexpr std::size_t kPerMessageOverhead = 4;

    std::size_t total = 0;
    for (const auto& m : msgs) {
        total += kPerMessageOverhead;
        if (const auto* s = std::get_if<std::string>(&m.content)) {
            total += s->size() / kCharsPerToken;
        } else if (const auto* parts = std::get_if<std::vector<ContentPart>>(&m.content)) {
            for (const auto& p : *parts) {
                total += p.text.size() / kCharsPerToken;
                if (p.image_url) total += p.image_url->url.size() / kCharsPerToken;
            }
        }
        for (const auto& tc : m.tool_calls) {
            total += tc.name.size() / kCharsPerToken;
            total += tc.arguments.dump().size() / kCharsPerToken;
        }
        if (m.name) total += m.name->size() / kCharsPerToken;
    }
    return total;
}

struct TokenBudget {
    // The ceiling. Compaction starts once the conversation crosses it.
    std::size_t max_tokens = 8000;
    // Where to compact down to, as a fraction of max_tokens. Compacting exactly
    // to the ceiling means compacting again on the very next turn; leaving
    // headroom buys several turns per pass. 0.6 is a reasonable default.
    double target_ratio = 0.6;
    // Messages at the tail that no stage may touch. The model needs the recent
    // turns verbatim to continue coherently.
    std::size_t keep_recent = 4;
    // Leave the leading system message in place.
    bool keep_system = true;
    // Empty means approx_token_count.
    TokenCounter count;

    [[nodiscard]] std::size_t target_tokens() const {
        auto ratio = target_ratio <= 0.0 || target_ratio > 1.0 ? 0.6 : target_ratio;
        return static_cast<std::size_t>(static_cast<double>(max_tokens) * ratio);
    }

    [[nodiscard]] std::size_t measure(const std::vector<Message>& msgs) const {
        return count ? count(msgs) : approx_token_count(msgs);
    }

    [[nodiscard]] bool over_budget(const std::vector<Message>& msgs) const {
        return measure(msgs) > max_tokens;
    }

    [[nodiscard]] bool within_target(const std::vector<Message>& msgs) const {
        return measure(msgs) <= target_tokens();
    }
};

// ─── Message-vector primitives ────────────────────────────────────────────────

namespace context {

// Marks the message a summarization stage produced. A summary stands in for
// every message already folded away, so it is the densest thing in the prompt
// and the worst possible candidate for the next stage to drop — without this
// marker, summarizing and then trimming pays for a summary and immediately
// throws it out.
inline constexpr const char* kSummaryMarker = "context_summary";

inline bool is_summary(const Message& m) {
    return m.role == Role::system && m.name && *m.name == kSummaryMarker;
}

// How many leading messages are off limits: the system prompt, plus any summary
// sitting behind it.
inline std::size_t protected_prefix(const std::vector<Message>& msgs, bool keep_system) {
    std::size_t n = 0;
    if (keep_system && !msgs.empty() && msgs.front().role == Role::system) n = 1;
    while (n < msgs.size() && is_summary(msgs[n])) ++n;
    return n;
}

// Moves a cut point forward past any tool results that would be left without the
// assistant message that requested them. Cutting at the returned index always
// yields a well-formed conversation.
inline std::size_t snap_past_orphan_tools(const std::vector<Message>& msgs,
                                          std::size_t cut) {
    while (cut < msgs.size() && msgs[cut].role == Role::tool) ++cut;
    return cut;
}

// Where the protected tail starts, snapped so it never begins with an orphan.
inline std::size_t tail_start(const std::vector<Message>& msgs, std::size_t keep_recent) {
    if (keep_recent >= msgs.size()) return 0;
    return snap_past_orphan_tools(msgs, msgs.size() - keep_recent);
}

// ── Stage 1: blank old tool output ───────────────────────────────────────────
//
// Tool results are usually the bulk of a long agent conversation and the least
// useful part once the model has acted on them. Replacing the body keeps the
// message — and so the tool-call pairing — while dropping nearly all its cost.
// Returns whether anything changed.
inline bool clear_tool_results(std::vector<Message>& msgs,
                               std::size_t keep_recent_results,
                               const std::string& placeholder,
                               bool clear_tool_inputs = false) {
    std::size_t total = 0;
    for (const auto& m : msgs)
        if (m.role == Role::tool) ++total;
    if (total <= keep_recent_results) return false;

    auto to_clear = total - keep_recent_results;
    bool changed = false;
    std::size_t cleared = 0;

    for (auto& m : msgs) {
        if (cleared >= to_clear) break;
        if (m.role != Role::tool) continue;
        if (m.text() == placeholder) { ++cleared; continue; }   // already done
        m.content = placeholder;
        ++cleared;
        changed = true;
    }

    if (clear_tool_inputs) {
        std::size_t remaining = to_clear;
        for (auto& m : msgs) {
            if (remaining == 0) break;
            if (m.role != Role::assistant) continue;
            for (auto& tc : m.tool_calls) {
                if (remaining == 0) break;
                if (!tc.arguments.empty()) { tc.arguments = json::object(); changed = true; }
                --remaining;
            }
        }
    }
    return changed;
}

// ── Stage 2: fold the middle into a summary ──────────────────────────────────
//
// Keeps the system prompt and the recent tail verbatim, replaces everything
// between them with one system message. Returns whether anything changed.
inline bool summarize_middle(std::vector<Message>& msgs,
                             std::size_t keep_recent,
                             const SummarizerFn& summarizer,
                             const std::string& prefix,
                             bool keep_system = true) {
    if (!summarizer) return false;
    auto start = protected_prefix(msgs, keep_system);
    auto end   = tail_start(msgs, keep_recent);
    // Nothing worth summarizing unless at least two messages sit in the middle;
    // replacing one message with one summary is not a saving.
    if (end <= start + 1) return false;

    std::vector<Message> middle(msgs.begin() + static_cast<long>(start),
                                msgs.begin() + static_cast<long>(end));
    auto summary = summarizer(middle);
    if (summary.empty()) return false;

    auto summary_msg = Message::system(prefix + summary);
    summary_msg.name = kSummaryMarker;   // so later stages will not drop it

    std::vector<Message> out;
    out.reserve(msgs.size() - middle.size() + 1);
    for (std::size_t i = 0; i < start; ++i) out.push_back(std::move(msgs[i]));
    out.push_back(std::move(summary_msg));
    for (std::size_t i = end; i < msgs.size(); ++i) out.push_back(std::move(msgs[i]));
    msgs = std::move(out);
    return true;
}

// ── Stage 3: drop the oldest ─────────────────────────────────────────────────
//
// Removes messages from the front, after the protected prefix, until the
// conversation is under target or only the protected tail is left. The cut snaps
// forward past orphan tool results, so it may remove one more message than the
// arithmetic asks for. Returns whether anything changed.
inline bool trim_oldest(std::vector<Message>& msgs, const TokenBudget& budget) {
    auto start = protected_prefix(msgs, budget.keep_system);
    auto floor = tail_start(msgs, budget.keep_recent);
    if (floor <= start) return false;

    auto target = budget.target_tokens();
    bool changed = false;

    while (budget.measure(msgs) > target) {
        auto keep_from = start + 1;
        keep_from = snap_past_orphan_tools(msgs, keep_from);
        auto tail = tail_start(msgs, budget.keep_recent);
        if (keep_from > tail || keep_from <= start) break;

        msgs.erase(msgs.begin() + static_cast<long>(start),
                   msgs.begin() + static_cast<long>(keep_from));
        changed = true;
    }
    return changed;
}

// Drops messages until at most max_messages remain (not counting the system
// prompt), snapping past orphan tool results. This is what trim_history() means
// by a limit: a message count, not a token count.
inline bool trim_to_message_count(std::vector<Message>& msgs,
                                  std::size_t max_messages,
                                  bool keep_system = true) {
    auto start = protected_prefix(msgs, keep_system);
    if (msgs.size() - start <= max_messages) return false;

    auto excess = msgs.size() - start - max_messages;
    auto cut = snap_past_orphan_tools(msgs, start + excess);
    if (cut <= start) return false;

    msgs.erase(msgs.begin() + static_cast<long>(start),
               msgs.begin() + static_cast<long>(cut));
    return true;
}

} // namespace context

// ─── Default summarizer ───────────────────────────────────────────────────────

// Extractive: no model call, no network. Truncates each message and labels it by
// role. Tool results get a third of the budget, since they are the most verbose
// and the least worth quoting.
inline std::string extractive_summarize(const std::vector<Message>& msgs,
                                        std::size_t max_per_msg = 150) {
    std::string out;
    out.reserve(msgs.size() * (max_per_msg + 20));
    for (const auto& m : msgs) {
        auto t = m.text();
        if (t.empty() && m.has_tool_calls()) {
            // An assistant turn that only called tools still carries meaning.
            t = "called ";
            for (std::size_t i = 0; i < m.tool_calls.size(); ++i) {
                if (i) t += ", ";
                t += m.tool_calls[i].name;
            }
        }
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

// ─── context_management ───────────────────────────────────────────────────────

struct ContextConfig {
    TokenBudget budget;

    // Stage 1
    bool        clear_tool_results   = true;
    std::size_t keep_tool_results    = 3;
    std::string tool_placeholder     = "[tool output cleared to stay within the context budget]";
    bool        clear_tool_inputs    = false;

    // Stage 2. Leave summarizer empty for the extractive default; pass
    // llm_summarizer() from summarize.hpp to have a model write it instead.
    bool         summarize           = true;
    SummarizerFn summarizer;
    std::string  summary_prefix      = "[Conversation summary]\n";

    // Stage 3
    bool trim = true;

    Log log;
};

// One middleware for the whole story. Measures, and if the conversation is over
// budget escalates through the stages until it fits or there is nothing left to
// give. Under budget it does nothing at all, which is the common case.
inline MiddlewareFn context_management(ContextConfig cfg = {}) {
    if (!cfg.summarizer)
        cfg.summarizer = [](const std::vector<Message>& m) { return extractive_summarize(m); };

    return [cfg = std::move(cfg)](std::vector<Message>& msgs, Next next) -> LLMResponse {
        if (!cfg.budget.over_budget(msgs)) return next(msgs);

        auto before = cfg.budget.measure(msgs);
        cfg.log.debug("context", "over budget: " + std::to_string(before) + " > "
            + std::to_string(cfg.budget.max_tokens) + " tokens, compacting to "
            + std::to_string(cfg.budget.target_tokens()));

        if (cfg.clear_tool_results
            && context::clear_tool_results(msgs, cfg.keep_tool_results,
                                           cfg.tool_placeholder, cfg.clear_tool_inputs)) {
            cfg.log.debug("context", "cleared old tool output: "
                + std::to_string(cfg.budget.measure(msgs)) + " tokens");
        }

        if (!cfg.budget.within_target(msgs) && cfg.summarize
            && context::summarize_middle(msgs, cfg.budget.keep_recent, cfg.summarizer,
                                         cfg.summary_prefix, cfg.budget.keep_system)) {
            cfg.log.debug("context", "summarized the middle: "
                + std::to_string(cfg.budget.measure(msgs)) + " tokens");
        }

        if (!cfg.budget.within_target(msgs) && cfg.trim
            && context::trim_oldest(msgs, cfg.budget)) {
            cfg.log.debug("context", "dropped the oldest messages: "
                + std::to_string(cfg.budget.measure(msgs)) + " tokens");
        }

        auto after = cfg.budget.measure(msgs);
        if (after > cfg.budget.max_tokens)
            cfg.log.warn("context", "still over budget after compacting ("
                + std::to_string(after) + " > " + std::to_string(cfg.budget.max_tokens)
                + " tokens); the protected tail alone exceeds it");
        else
            cfg.log.info("context", "compacted " + std::to_string(before) + " -> "
                + std::to_string(after) + " tokens");

        return next(msgs);
    };
}

} // namespace tiny_agent::middleware
