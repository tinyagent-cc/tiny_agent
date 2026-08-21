#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  observability/console.hpp  —  exporters that need no backend
//
//  StreamExporter prints one line per span to any ostream, defaulting to stderr.
//  It is what you reach for when tracing is misbehaving and you want to see the
//  spans before deciding whether the problem is the agent or the collector.
// ═══════════════════════════════════════════════════════════════════════════════

#include "trace.hpp"
#include <iomanip>
#include <iostream>
#include <ostream>

namespace tiny_agent::obs {

struct StreamExporterConfig {
    // Include the prompt and completion text. Off by default: a console trace
    // is often pasted into a bug report.
    bool        verbose = false;
    std::size_t max_content_chars = 200;
};

class StreamExporter final : public Exporter {
    std::reference_wrapper<std::ostream> os_;
    StreamExporterConfig cfg_;
    std::mutex           mu_;

    static std::string status_word(SpanStatus s) {
        switch (s) {
            case SpanStatus::ok:    return "ok";
            case SpanStatus::error: return "error";
            case SpanStatus::unset: return "unset";
        }
        return "unset";
    }

    std::string clip(const std::string& s) const {
        if (s.size() <= cfg_.max_content_chars) return s;
        return s.substr(0, cfg_.max_content_chars) + "…";
    }

public:
    explicit StreamExporter(std::ostream& os = std::cerr, StreamExporterConfig cfg = {})
        : os_(os), cfg_(cfg) {}

    void export_spans(const std::vector<Span>& spans) override {
        std::lock_guard lock(mu_);
        auto& os = os_.get();
        for (const auto& s : spans) {
            auto ms = static_cast<double>(s.duration_nanos()) / 1e6;
            os << "[trace] " << s.trace_id.substr(0, 8) << "/" << s.span_id.substr(0, 8)
               << " " << to_string(s.kind) << " " << s.name
               << " " << std::fixed << std::setprecision(1) << ms << "ms"
               << " status=" << status_word(s.status);
            if (!s.model.empty()) os << " model=" << s.model;
            if (s.total_tokens)   os << " tokens=" << s.total_tokens;
            else if (s.input_tokens || s.output_tokens)
                os << " tokens=" << (s.input_tokens + s.output_tokens);
            if (!s.status_message.empty()) os << " error=\"" << s.status_message << "\"";
            os << "\n";
            if (cfg_.verbose) {
                if (!s.input.empty())  os << "         in:  " << clip(s.input)  << "\n";
                if (!s.output.empty()) os << "         out: " << clip(s.output) << "\n";
            }
        }
        os.flush();
    }

    [[nodiscard]] std::string name() const override { return "stream"; }
};

inline ExporterPtr stderr_exporter(StreamExporterConfig cfg = {}) {
    return std::make_shared<StreamExporter>(std::cerr, cfg);
}

inline ExporterPtr noop_exporter() {
    return std::make_shared<NoopExporter>();
}

// Keeps every span in memory. Tests assert against this instead of a server.
class MemoryExporter final : public Exporter {
    mutable std::mutex mu_;
    std::vector<Span>  spans_;

public:
    void export_spans(const std::vector<Span>& spans) override {
        std::lock_guard lock(mu_);
        spans_.insert(spans_.end(), spans.begin(), spans.end());
    }

    [[nodiscard]] std::vector<Span> spans() const {
        std::lock_guard lock(mu_);
        return spans_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mu_);
        return spans_.size();
    }

    void clear() {
        std::lock_guard lock(mu_);
        spans_.clear();
    }

    [[nodiscard]] std::string name() const override { return "memory"; }
};

} // namespace tiny_agent::obs
