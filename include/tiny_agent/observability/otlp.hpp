#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  observability/otlp.hpp  —  OTLP/HTTP with a JSON body
//
//  The OTLP spec defines two encodings for the same payload: protobuf and
//  proto3-JSON. tiny_agent emits the JSON one, because it needs nothing beyond
//  nlohmann::json and the httplib client already in the build. Every collector
//  and vendor endpoint that speaks OTLP/HTTP accepts it under
//  Content-Type: application/json.
//
//  Two details of proto3-JSON matter and are easy to get wrong:
//   - traceId and spanId are hex strings here, not the base64 that plain
//     proto3-JSON would produce for a bytes field. OTLP calls this out as a
//     deliberate deviation.
//   - The *UnixNano fields are uint64, so they go on the wire as strings.
// ═══════════════════════════════════════════════════════════════════════════════

#include "trace.hpp"
#include <httplib.h>
#include <string>

namespace tiny_agent::obs {

// Which attribute vocabulary to write. Backends ignore attributes they do not
// recognize, so `both` is the default: one span shape that reads correctly in
// an OpenInference UI (Phoenix) and an OTel GenAI one (Langfuse) alike.
enum class SemConv { openinference, otel_genai, both };

namespace detail {

inline json attr(std::string key, std::string value) {
    return {{"key", std::move(key)}, {"value", {{"stringValue", std::move(value)}}}};
}

inline json attr_int(std::string key, std::int64_t value) {
    // int64 in proto3-JSON is a string.
    return {{"key", std::move(key)},
            {"value", {{"intValue", std::to_string(value)}}}};
}

// Splits "https://host:port/some/path" into the origin httplib::Client wants and
// the path it needs on each request.
struct Url { std::string origin; std::string path; };

inline Url split_url(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw Error("OTLP endpoint must include a scheme: " + url);
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {url, "/"};
    return {url.substr(0, path_start), url.substr(path_start)};
}

inline int span_kind_to_otlp(SpanKind k) {
    // SPAN_KIND_CLIENT for calls that leave the process, SPAN_KIND_INTERNAL for
    // orchestration. The OTel GenAI conventions ask for CLIENT on model calls.
    switch (k) {
        case SpanKind::llm:
        case SpanKind::embedding:
        case SpanKind::retriever: return 3;   // SPAN_KIND_CLIENT
        default:                  return 1;   // SPAN_KIND_INTERNAL
    }
}

inline int status_to_otlp(SpanStatus s) {
    switch (s) {
        case SpanStatus::ok:    return 1;   // STATUS_CODE_OK
        case SpanStatus::error: return 2;   // STATUS_CODE_ERROR
        case SpanStatus::unset: return 0;   // STATUS_CODE_UNSET
    }
    return 0;
}

inline void append_openinference(json& attrs, const Span& s) {
    attrs.push_back(attr("openinference.span.kind", to_string(s.kind)));
    if (!s.model.empty())         attrs.push_back(attr("llm.model_name", s.model));
    if (!s.system.empty())        attrs.push_back(attr("llm.system", s.system));
    if (!s.input.empty()) {
        attrs.push_back(attr("input.value", s.input));
        attrs.push_back(attr("input.mime_type", s.input_mime_type));
    }
    if (!s.output.empty()) {
        attrs.push_back(attr("output.value", s.output));
        attrs.push_back(attr("output.mime_type", s.output_mime_type));
    }
    if (!s.finish_reason.empty()) attrs.push_back(attr("llm.finish_reason", s.finish_reason));
    if (s.input_tokens)  attrs.push_back(attr_int("llm.token_count.prompt", s.input_tokens));
    if (s.output_tokens) attrs.push_back(attr_int("llm.token_count.completion", s.output_tokens));
    if (s.total_tokens)  attrs.push_back(attr_int("llm.token_count.total", s.total_tokens));
}

inline void append_otel_genai(json& attrs, const Span& s) {
    attrs.push_back(attr("langfuse.observation.type", to_observation_type(s.kind)));
    if (!s.system.empty()) {
        // gen_ai.provider.name is the current upstream name; gen_ai.system is
        // the one backends still read. Send both and let each take what it
        // knows.
        attrs.push_back(attr("gen_ai.system", s.system));
        attrs.push_back(attr("gen_ai.provider.name", s.system));
    }
    if (!s.model.empty()) {
        attrs.push_back(attr("gen_ai.request.model", s.model));
        attrs.push_back(attr("langfuse.observation.model.name", s.model));
    }
    if (!s.input.empty())  attrs.push_back(attr("langfuse.observation.input", s.input));
    if (!s.output.empty()) attrs.push_back(attr("langfuse.observation.output", s.output));
    if (!s.finish_reason.empty())
        attrs.push_back(attr("gen_ai.response.finish_reasons", s.finish_reason));
    if (s.input_tokens)  attrs.push_back(attr_int("gen_ai.usage.input_tokens", s.input_tokens));
    if (s.output_tokens) attrs.push_back(attr_int("gen_ai.usage.output_tokens", s.output_tokens));
}

} // namespace detail

// ─── to_otlp_json ─────────────────────────────────────────────────────────────
//
// Pure: spans in, request body out. No network, so the wire format is testable
// on its own.
inline json to_otlp_json(const std::vector<Span>& spans,
                         const std::string& service_name,
                         SemConv semconv = SemConv::both,
                         const std::map<std::string, std::string>& resource_attrs = {}) {
    json otlp_spans = json::array();
    for (const auto& s : spans) {
        json attrs = json::array();
        if (semconv == SemConv::openinference || semconv == SemConv::both)
            detail::append_openinference(attrs, s);
        if (semconv == SemConv::otel_genai || semconv == SemConv::both)
            detail::append_otel_genai(attrs, s);
        for (const auto& [k, v] : s.attributes)
            attrs.push_back(detail::attr(k, v));

        json span;
        span["traceId"]           = s.trace_id;
        span["spanId"]            = s.span_id;
        if (!s.parent_span_id.empty()) span["parentSpanId"] = s.parent_span_id;
        span["name"]              = s.name;
        span["kind"]              = detail::span_kind_to_otlp(s.kind);
        span["startTimeUnixNano"] = std::to_string(s.start_unix_nano);
        span["endTimeUnixNano"]   = std::to_string(s.end_unix_nano);
        span["attributes"]        = std::move(attrs);
        span["status"]            = {{"code", detail::status_to_otlp(s.status)}};
        if (!s.status_message.empty()) span["status"]["message"] = s.status_message;
        otlp_spans.push_back(std::move(span));
    }

    json resource = json::array();
    resource.push_back(detail::attr("service.name", service_name));
    for (const auto& [k, v] : resource_attrs)
        resource.push_back(detail::attr(k, v));

    return {{"resourceSpans", json::array({{
        {"resource", {{"attributes", std::move(resource)}}},
        {"scopeSpans", json::array({{
            {"scope", {{"name", "tiny_agent"}, {"version", "0.4.0"}}},
            {"spans", std::move(otlp_spans)}
        }})}
    }})}};
}

// ─── OtlpHttpExporter ─────────────────────────────────────────────────────────

struct OtlpConfig {
    // Full URL including the path, e.g. "http://localhost:6006/v1/traces".
    std::string endpoint;
    std::map<std::string, std::string> headers;
    std::string service_name = "tiny_agent";
    std::map<std::string, std::string> resource_attributes;
    SemConv     semconv = SemConv::both;
    int         timeout_seconds = 10;
    std::string label = "otlp";
    Log         log;
};

class OtlpHttpExporter final : public Exporter {
    OtlpConfig      cfg_;
    detail::Url     url_;
    httplib::Client client_;

public:
    explicit OtlpHttpExporter(OtlpConfig cfg)
        : cfg_(std::move(cfg))
        , url_(detail::split_url(cfg_.endpoint))
        , client_(url_.origin)
    {
        client_.set_read_timeout(cfg_.timeout_seconds);
        client_.set_write_timeout(cfg_.timeout_seconds);
        httplib::Headers hdrs;
        for (const auto& [k, v] : cfg_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    void export_spans(const std::vector<Span>& spans) override {
        if (spans.empty()) return;
        auto body = to_otlp_json(spans, cfg_.service_name, cfg_.semconv,
                                 cfg_.resource_attributes).dump();
        cfg_.log.debug("tracing", cfg_.label + ": POST " + url_.path + " ("
            + std::to_string(spans.size()) + " span(s), "
            + std::to_string(body.size()) + " bytes)");

        auto res = client_.Post(url_.path, body, "application/json");
        if (!res)
            throw Error(cfg_.label + " export failed: "
                + httplib::to_string(res.error()));
        if (res->status < 200 || res->status >= 300)
            throw Error(cfg_.label + " export rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));

        cfg_.log.trace("tracing", cfg_.label + ": accepted " + std::to_string(res->status));
    }

    [[nodiscard]] std::string name() const override { return cfg_.label; }
};

inline ExporterPtr otlp_exporter(OtlpConfig cfg) {
    return std::make_shared<OtlpHttpExporter>(std::move(cfg));
}

} // namespace tiny_agent::obs
