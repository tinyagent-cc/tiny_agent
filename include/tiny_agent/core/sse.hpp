#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  sse.hpp  —  Incremental Server-Sent Events parser
//
//  A standalone, transport-agnostic parser.  Feed it raw byte chunks as they
//  arrive off the wire; it emits one sse::Event per completed event.  It knows
//  nothing about JSON, providers, or HTTP — interpretation (including the
//  "[DONE]" sentinel) is entirely the caller's job.
//
//  Follows the WHATWG event-stream rules: fields split mid-line across chunks,
//  multiple events per chunk, "\n"/"\r\n"/"\r" line endings, multi-line "data:"
//  fields joined with newlines, comment lines (":" prefix) ignored, and a
//  trailing unterminated event flushed via finish().
// ═══════════════════════════════════════════════════════════════════════════════

#include <functional>
#include <string>
#include <string_view>

namespace tiny_agent::sse {

// ─── Event ──────────────────────────────────────────────────────────────────

struct Event {
    std::string event;   // the "event:" field ("" when unset)
    std::string data;    // the joined "data:" payload, trailing newline stripped
};

// ─── Parser ─────────────────────────────────────────────────────────────────

class Parser {
    std::string pending_;   // bytes not yet forming a complete line
    std::string event_;     // current event-type buffer
    std::string data_;      // current data buffer ("value\n" per data line)

public:
    using Sink = std::function<void(Event)>;

    // Feed a raw chunk; invokes sink once per completed event.
    void feed(std::string_view chunk, const Sink& sink) {
        pending_.append(chunk.data(), chunk.size());
        std::size_t start = 0;
        for (;;) {
            auto nl = pending_.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = pending_.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            process_line(line, sink);
            start = nl + 1;
        }
        pending_.erase(0, start);
    }

    // Flush a trailing event that never received its blank-line terminator.
    void finish(const Sink& sink) {
        if (!pending_.empty()) {
            std::string line = std::move(pending_);
            pending_.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            process_line(line, sink);
        }
        dispatch(sink);
    }

private:
    void process_line(const std::string& line, const Sink& sink) {
        if (line.empty()) { dispatch(sink); return; }   // blank line → dispatch
        if (line[0] == ':') return;                     // comment → ignore

        auto colon = line.find(':');
        std::string field, value;
        if (colon == std::string::npos) {
            field = line;                               // field name with no value
        } else {
            field = line.substr(0, colon);
            value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.erase(0, 1);
        }

        if (field == "event")      event_ = value;
        else if (field == "data")  { data_ += value; data_ += '\n'; }
        // "id", "retry", and unknown fields are ignored.
    }

    void dispatch(const Sink& sink) {
        if (data_.empty()) { event_.clear(); return; }  // nothing accumulated
        if (data_.back() == '\n') data_.pop_back();      // strip one trailing "\n"
        sink(Event{event_, data_});
        event_.clear();
        data_.clear();
    }
};

} // namespace tiny_agent::sse
