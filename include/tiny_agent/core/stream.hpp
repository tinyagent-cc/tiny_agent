#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  stream.hpp  —  Provider-agnostic streaming event model
//
//  StreamEvent      — one normalized delta (text, tool-call fragment, finish…).
//                     Providers translate their wire format into these; every
//                     consumer sees only this type.
//  StreamHandler    — the live callback fed each StreamEvent as it arrives.
//  StreamAccumulator— folds a sequence of StreamEvents into a final LLMResponse
//                     (message text, assembled tool calls, finish reason).  Pure:
//                     testable without any network.
//
//  Tool-call ids are intentionally NOT carried on StreamEvent — a provider
//  decoder stashes them and writes them back onto the folded response via its
//  own restore_ids() (see restore_tool_ids below).
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tiny_agent {

// ─── StreamEvent ──────────────────────────────────────────────────────────────

struct StreamEvent {
    enum class Kind { message_start, text_delta, tool_call_delta, finish };
    Kind        kind;
    std::string text;              // text_delta: the token(s)
    int         tool_index = -1;   // tool_call_delta: which call
    std::string tool_name;         // tool_call_delta: name fragment (may be empty)
    std::string tool_args;         // tool_call_delta: arguments JSON fragment
    std::string finish_reason;     // finish
};

using StreamHandler = std::function<void(const StreamEvent&)>;

// ─── StreamAccumulator ────────────────────────────────────────────────────────

class StreamAccumulator {
    struct ToolAcc { int index; std::string name; std::string args; };

    std::string           text_;
    std::string           finish_reason_;
    std::vector<ToolAcc>  tools_;   // in first-appearance (== ascending index) order

    ToolAcc& slot_for(int index) {
        for (auto& t : tools_)
            if (t.index == index) return t;
        tools_.push_back({index, {}, {}});
        return tools_.back();
    }

public:
    void push(const StreamEvent& e) {
        switch (e.kind) {
            case StreamEvent::Kind::message_start:
                break;
            case StreamEvent::Kind::text_delta:
                text_ += e.text;
                break;
            case StreamEvent::Kind::tool_call_delta: {
                auto& t = slot_for(e.tool_index);
                t.name += e.tool_name;
                t.args += e.tool_args;
                break;
            }
            case StreamEvent::Kind::finish:
                if (!e.finish_reason.empty()) finish_reason_ = e.finish_reason;
                break;
        }
    }

    [[nodiscard]] LLMResponse result() const {
        Message m;
        m.role    = Role::assistant;
        m.content = text_;
        for (auto& t : tools_) {
            // A stream can be cut off mid-arguments, leaving unparseable JSON.
            // Preserve the fragment under _raw_arguments so the tool call fails
            // with a message the model can act on, rather than throwing here and
            // discarding a response that is otherwise complete.
            json args = json::object();
            if (!t.args.empty()) {
                try { args = json::parse(t.args); }
                catch (const std::exception&) { args = json{{"_raw_arguments", t.args}}; }
            }
            m.tool_calls.push_back({std::string{}, t.name, std::move(args)});
        }
        LLMResponse r;
        r.message       = std::move(m);
        r.finish_reason = finish_reason_;
        return r;
    }
};

// ─── restore_tool_ids ─────────────────────────────────────────────────────────
//
// Shared by the provider decoders: zips ids (keyed by tool/block index, so
// iterated in ascending index order) onto the accumulated tool calls, which the
// accumulator built in that same order.

namespace detail {
inline void restore_tool_ids(LLMResponse& r, const std::map<int, std::string>& ids) {
    std::size_t i = 0;
    for (auto& [index, id] : ids) {
        if (i < r.message.tool_calls.size()) r.message.tool_calls[i].id = id;
        ++i;
    }
}
} // namespace detail

} // namespace tiny_agent
