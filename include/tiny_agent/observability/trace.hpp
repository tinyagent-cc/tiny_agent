#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  observability/trace.hpp  —  spans, exporters, and the tracer that joins them
//
//  The shape here is deliberately small: a Span is a plain struct of the facts
//  an LLM call produces, and an Exporter is one method that receives a batch of
//  them. Everything vendor-specific lives in an exporter, so a monitoring
//  backend is an include and a config struct, never a build dependency.
//
//  Nothing in this header is reachable unless you include it. tiny_agent.hpp
//  does not pull it in.
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/types.hpp"
#include "../core/log.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace tiny_agent::obs {

// ─── Span ─────────────────────────────────────────────────────────────────────

// What kind of work the span covers. Exporters map this onto whatever their
// backend calls the same idea (OpenInference span kinds, Langfuse observation
// types), so the framework never has to name a vendor.
enum class SpanKind { llm, agent, tool, chain, retriever, embedding };

enum class SpanStatus { unset, ok, error };

constexpr const char* to_string(SpanKind k) noexcept {
    switch (k) {
        case SpanKind::llm:       return "LLM";
        case SpanKind::agent:     return "AGENT";
        case SpanKind::tool:      return "TOOL";
        case SpanKind::chain:     return "CHAIN";
        case SpanKind::retriever: return "RETRIEVER";
        case SpanKind::embedding: return "EMBEDDING";
    }
    return "CHAIN";
}

// The Langfuse observation-type vocabulary for the same kinds.
constexpr const char* to_observation_type(SpanKind k) noexcept {
    switch (k) {
        case SpanKind::llm:       return "generation";
        case SpanKind::agent:     return "agent";
        case SpanKind::tool:      return "tool";
        case SpanKind::chain:     return "chain";
        case SpanKind::retriever: return "retriever";
        case SpanKind::embedding: return "embedding";
    }
    return "span";
}

struct Span {
    // W3C trace context: trace_id is 32 lowercase hex characters, span_id and
    // parent_span_id are 16. Exporters rely on that; new_trace_id() and
    // new_span_id() below are the way to get conforming values.
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;

    std::string   name;
    SpanKind      kind   = SpanKind::llm;
    SpanStatus    status = SpanStatus::unset;
    std::string   status_message;
    std::uint64_t start_unix_nano = 0;
    std::uint64_t end_unix_nano   = 0;

    // GenAI payload, in backend-neutral terms.
    std::string  model;
    std::string  system;          // "openai", "anthropic", "gemini", …
    std::string  input;           // serialized prompt / arguments
    std::string  output;          // serialized completion / result
    std::string  input_mime_type  = "application/json";
    std::string  output_mime_type = "text/plain";
    std::string  finish_reason;
    std::int64_t input_tokens  = 0;
    std::int64_t output_tokens = 0;
    std::int64_t total_tokens  = 0;

    // Extra string-valued attributes, passed through verbatim. Use this for
    // session ids, user ids, tags — anything a backend reads that the fields
    // above do not cover.
    std::map<std::string, std::string> attributes;

    [[nodiscard]] std::uint64_t duration_nanos() const {
        return end_unix_nano > start_unix_nano ? end_unix_nano - start_unix_nano : 0;
    }
};

// ─── Clock and id generation ──────────────────────────────────────────────────

inline std::uint64_t now_unix_nano() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

namespace detail {

inline std::mt19937_64& rng() {
    // One generator per thread: no lock on the hot path, and two threads never
    // draw the same id.
    thread_local std::mt19937_64 gen{std::random_device{}()};
    return gen;
}

inline std::string to_hex(std::uint64_t v, int digits) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[v & 0xF];
        v >>= 4;
    }
    return out;
}

} // namespace detail

inline std::string new_trace_id() {
    return detail::to_hex(detail::rng()(), 16) + detail::to_hex(detail::rng()(), 16);
}

inline std::string new_span_id() {
    return detail::to_hex(detail::rng()(), 16);
}

// ─── Exporter ─────────────────────────────────────────────────────────────────

// Runtime exporter interface. Spans arrive in batches; an exporter may block,
// and may throw — the tracer catches and logs rather than letting a monitoring
// outage propagate into the agent.
struct Exporter {
    virtual ~Exporter() = default;
    virtual void export_spans(const std::vector<Span>& spans) = 0;
    virtual void shutdown() {}
    [[nodiscard]] virtual std::string name() const { return "exporter"; }
};

using ExporterPtr = std::shared_ptr<Exporter>;

// Compile-time counterpart: anything with an export_spans(const vector<Span>&)
// can be wrapped without inheriting from Exporter.
template<typename T>
concept trace_exporter = requires(T& e, const std::vector<Span>& s) {
    e.export_spans(s);
};

template<trace_exporter T>
ExporterPtr make_exporter(T impl, std::string label = "exporter") {
    struct Adapter final : Exporter {
        T           impl_;
        std::string label_;
        Adapter(T i, std::string l) : impl_(std::move(i)), label_(std::move(l)) {}
        void export_spans(const std::vector<Span>& spans) override { impl_.export_spans(spans); }
        [[nodiscard]] std::string name() const override { return label_; }
    };
    return std::make_shared<Adapter>(std::move(impl), std::move(label));
}

// Discards everything. The default, so tracing code paths stay live and
// measurable with no backend configured and no output.
struct NoopExporter final : Exporter {
    void export_spans(const std::vector<Span>&) override {}
    [[nodiscard]] std::string name() const override { return "noop"; }
};

// ─── Tracer ───────────────────────────────────────────────────────────────────

struct TracerConfig {
    std::string service_name = "tiny_agent";
    // Spans buffer until this many are pending, then go out in one call. Set to
    // 1 to export each span as it closes.
    std::size_t max_batch = 32;
    Log         log;
};

// Buffers spans and hands them to an exporter. Safe to share between threads:
// record() and flush() are serialized, and record() copies the span in.
//
// An exporter that throws never reaches the caller — the batch is dropped and
// the failure is logged at error level. Losing traces is the correct failure
// mode for observability; losing the agent run is not.
class Tracer {
    ExporterPtr        exporter_;
    TracerConfig       cfg_;
    mutable std::mutex mu_;
    std::vector<Span>  buffer_;

    void export_locked(std::vector<Span> batch) {
        if (batch.empty()) return;
        try {
            exporter_->export_spans(batch);
        } catch (const std::exception& e) {
            cfg_.log.error("tracing", "exporter '" + exporter_->name()
                + "' failed, dropping " + std::to_string(batch.size())
                + " span(s): " + e.what());
        } catch (...) {
            cfg_.log.error("tracing", "exporter '" + exporter_->name()
                + "' failed with an unknown exception, dropping "
                + std::to_string(batch.size()) + " span(s)");
        }
    }

public:
    explicit Tracer(ExporterPtr exporter = nullptr, TracerConfig cfg = {})
        : exporter_(exporter ? std::move(exporter) : std::make_shared<NoopExporter>())
        , cfg_(std::move(cfg))
    {
        if (cfg_.max_batch == 0) cfg_.max_batch = 1;
    }

    ~Tracer() {
        try {
            flush();
            exporter_->shutdown();
        } catch (...) {}   // a destructor must not throw
    }

    Tracer(const Tracer&)            = delete;
    Tracer& operator=(const Tracer&) = delete;

    void record(Span s) {
        std::vector<Span> batch;
        {
            std::lock_guard lock(mu_);
            buffer_.push_back(std::move(s));
            if (buffer_.size() < cfg_.max_batch) return;
            batch.swap(buffer_);
        }
        export_locked(std::move(batch));
    }

    void flush() {
        std::vector<Span> batch;
        {
            std::lock_guard lock(mu_);
            batch.swap(buffer_);
        }
        export_locked(std::move(batch));
    }

    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock(mu_);
        return buffer_.size();
    }

    [[nodiscard]] const std::string& service_name() const { return cfg_.service_name; }
    [[nodiscard]] const ExporterPtr& exporter() const { return exporter_; }
};

using TracerPtr = std::shared_ptr<Tracer>;

// ─── Ambient trace context ────────────────────────────────────────────────────

// Middleware sees one model call at a time and has no idea where an agent run
// begins or ends. ScopedTrace supplies that boundary: open one around
// agent.run(...) and every span the middleware creates while it is alive joins
// the same trace as a child. Without one, each model call is its own one-span
// trace, which is still useful but loses the run-level grouping.
//
// The context is thread_local, matching the rule that an agent instance belongs
// to one thread.
namespace context {

struct Frame {
    std::string trace_id;
    std::string span_id;
};

inline std::vector<Frame>& stack() {
    thread_local std::vector<Frame> frames;
    return frames;
}

inline bool active() { return !stack().empty(); }

inline std::string current_trace_id() {
    return stack().empty() ? std::string{} : stack().back().trace_id;
}

inline std::string current_span_id() {
    return stack().empty() ? std::string{} : stack().back().span_id;
}

} // namespace context

class ScopedTrace {
    TracerPtr tracer_;
    Span      span_;
    bool      open_ = false;

public:
    ScopedTrace(TracerPtr tracer, std::string name, SpanKind kind = SpanKind::agent)
        : tracer_(std::move(tracer))
    {
        span_.trace_id = context::active() ? context::current_trace_id() : new_trace_id();
        span_.parent_span_id  = context::current_span_id();
        span_.span_id         = new_span_id();
        span_.name            = std::move(name);
        span_.kind            = kind;
        span_.start_unix_nano = now_unix_nano();
        context::stack().push_back({span_.trace_id, span_.span_id});
        open_ = true;
    }

    ~ScopedTrace() {
        try { end(); } catch (...) {}
    }

    ScopedTrace(const ScopedTrace&)            = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;

    // Fill in the run's own input/output before it closes.
    Span& span() { return span_; }

    void set_error(const std::string& message) {
        span_.status = SpanStatus::error;
        span_.status_message = message;
    }

    void end() {
        if (!open_) return;
        open_ = false;
        if (!context::stack().empty()) context::stack().pop_back();
        span_.end_unix_nano = now_unix_nano();
        if (span_.status == SpanStatus::unset) span_.status = SpanStatus::ok;
        if (tracer_) tracer_->record(span_);
    }
};

} // namespace tiny_agent::obs
