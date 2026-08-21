#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  observability/langfuse.hpp  —  Langfuse
//
//  Langfuse exposes two ingestion paths. This targets the OTel one at
//  /api/public/otel/v1/traces, not the native /api/public/ingestion batch API:
//  Langfuse has deprecated the native endpoint, its Cloud sunset is 2026-11-16,
//  and self-hosted v4 turns it off under the default write mode. Building a
//  second wire format with that shelf life would be work with a known expiry
//  date, so tiny_agent ships one path and it is the one Langfuse recommends.
//
//    auto tracer = std::make_shared<obs::Tracer>(
//        obs::langfuse_exporter({.public_key = pk, .secret_key = sk}));
//
//  Auth is HTTP Basic over the public/secret key pair.
// ═══════════════════════════════════════════════════════════════════════════════

#include "otlp.hpp"

namespace tiny_agent::obs {

namespace detail {

inline std::string base64(const std::string& in) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        auto v = (static_cast<unsigned>(static_cast<unsigned char>(in[i]))     << 16)
               | (static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) <<  8)
               |  static_cast<unsigned>(static_cast<unsigned char>(in[i + 2]));
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >>  6) & 0x3F];
        out += kTable[ v        & 0x3F];
    }

    if (i < in.size()) {
        auto rem = in.size() - i;
        auto v = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16;
        if (rem == 2) v |= static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8;
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += rem == 2 ? kTable[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

} // namespace detail

struct LangfuseConfig {
    // Cloud EU is the default. US is "https://us.cloud.langfuse.com"; a
    // self-hosted deployment needs v3.22.0 or newer for the OTel endpoint.
    std::string base_url = "https://cloud.langfuse.com";
    std::string public_key;   // pk-lf-…
    std::string secret_key;   // sk-lf-…
    std::string service_name = "tiny_agent";
    // Landing on every span, not just the root: Langfuse reads trace-level
    // fields off whichever span it is processing.
    std::string session_id;
    std::string user_id;
    std::string trace_name;
    std::map<std::string, std::string> resource_attributes;
    int  timeout_seconds = 10;
    Log  log;
};

inline OtlpConfig langfuse_otlp_config(LangfuseConfig cfg) {
    if (cfg.public_key.empty() || cfg.secret_key.empty())
        throw Error("langfuse_exporter: both public_key and secret_key are required");

    auto base = cfg.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();

    OtlpConfig otlp;
    otlp.endpoint = base + "/api/public/otel/v1/traces";
    otlp.headers["Authorization"] =
        "Basic " + detail::base64(cfg.public_key + ":" + cfg.secret_key);
    // Opts into Langfuse's current ingestion behaviour rather than the legacy
    // compatibility path.
    otlp.headers["x-langfuse-ingestion-version"] = "4";
    otlp.service_name        = std::move(cfg.service_name);
    otlp.resource_attributes = std::move(cfg.resource_attributes);
    otlp.semconv             = SemConv::otel_genai;
    otlp.timeout_seconds     = cfg.timeout_seconds;
    otlp.label               = "langfuse";
    otlp.log                 = std::move(cfg.log);
    return otlp;
}

// Copies the trace-level identifiers onto a span. Call it from your own code, or
// let langfuse_span_decorator() below do it for every span the tracer records.
inline void apply_langfuse_trace_fields(Span& s, const LangfuseConfig& cfg) {
    if (!cfg.session_id.empty()) s.attributes["langfuse.session.id"] = cfg.session_id;
    if (!cfg.user_id.empty())    s.attributes["langfuse.user.id"]    = cfg.user_id;
    if (!cfg.trace_name.empty()) s.attributes["langfuse.trace.name"] = cfg.trace_name;
}

class LangfuseExporter final : public Exporter {
    LangfuseConfig     cfg_;
    OtlpHttpExporter   inner_;

public:
    explicit LangfuseExporter(LangfuseConfig cfg)
        : cfg_(cfg), inner_(langfuse_otlp_config(cfg)) {}

    void export_spans(const std::vector<Span>& spans) override {
        if (cfg_.session_id.empty() && cfg_.user_id.empty() && cfg_.trace_name.empty()) {
            inner_.export_spans(spans);
            return;
        }
        std::vector<Span> decorated = spans;
        for (auto& s : decorated) apply_langfuse_trace_fields(s, cfg_);
        inner_.export_spans(decorated);
    }

    [[nodiscard]] std::string name() const override { return "langfuse"; }
};

inline ExporterPtr langfuse_exporter(LangfuseConfig cfg) {
    return std::make_shared<LangfuseExporter>(std::move(cfg));
}

} // namespace tiny_agent::obs
