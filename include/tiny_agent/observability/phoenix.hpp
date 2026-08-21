#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  observability/phoenix.hpp  —  Arize Phoenix
//
//  Phoenix's OTLP collector at /v1/traces speaks protobuf only: posting an
//  OTLP/HTTP body with Content-Type application/json gets
//  "415 Unsupported content type: application/json" (verified against
//  arize-phoenix on 2026-08-21). Since tiny_agent will not take on a protobuf
//  dependency to send a trace, this exporter targets Phoenix's own span API
//  instead:
//
//      POST /v1/projects/{project}/spans   →  202 {"total_received":n,…}
//
//  It is JSON, it takes OpenInference attributes directly, and the spans land
//  in the same project view as OTLP-ingested ones.
//
//    auto tracer = std::make_shared<obs::Tracer>(
//        obs::phoenix_exporter({.base_url = "http://localhost:6006",
//                               .project_name = "my-agent"}));
//
//  Auth is only enforced when the deployment enables it; a bare local
//  `phoenix serve` accepts spans with no api_key.
// ═══════════════════════════════════════════════════════════════════════════════

#include "otlp.hpp"
#include <cstdio>
#include <ctime>

namespace tiny_agent::obs {

namespace detail {

// Phoenix wants RFC 3339 with an explicit zone ("must be timezone-aware"), to
// microsecond precision.
inline std::string to_rfc3339(std::uint64_t unix_nano) {
    auto secs  = static_cast<std::time_t>(unix_nano / 1'000'000'000ULL);
    auto micros = static_cast<unsigned>((unix_nano % 1'000'000'000ULL) / 1000ULL);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%06uZ", buf, micros);
    return out;
}

inline const char* phoenix_status(SpanStatus s) {
    switch (s) {
        case SpanStatus::ok:    return "OK";
        case SpanStatus::error: return "ERROR";
        case SpanStatus::unset: return "UNSET";
    }
    return "UNSET";
}

} // namespace detail

struct PhoenixConfig {
    // Self-hosted: "http://localhost:6006". Phoenix Cloud: the base URL for your
    // space, which for some tenants carries an "/s/<space>" prefix — paste it
    // whole, the API path is appended here.
    std::string base_url = "http://localhost:6006";
    std::string api_key;                       // "authorization: Bearer <key>"
    std::string project_name = "default";      // the project spans land in
    std::string service_name = "tiny_agent";
    int  timeout_seconds = 10;
    Log  log;
};

// Pure: spans in, request body out, so the wire format is testable offline.
inline json to_phoenix_json(const std::vector<Span>& spans,
                            const std::string& service_name) {
    json data = json::array();
    for (const auto& s : spans) {
        json attrs = json::object();
        attrs["openinference.span.kind"] = to_string(s.kind);
        attrs["service.name"]            = service_name;
        if (!s.model.empty())  attrs["llm.model_name"] = s.model;
        if (!s.system.empty()) attrs["llm.system"]     = s.system;
        if (!s.input.empty()) {
            attrs["input.value"]     = s.input;
            attrs["input.mime_type"] = s.input_mime_type;
        }
        if (!s.output.empty()) {
            attrs["output.value"]     = s.output;
            attrs["output.mime_type"] = s.output_mime_type;
        }
        if (!s.finish_reason.empty()) attrs["llm.finish_reason"] = s.finish_reason;
        // Phoenix takes native JSON types here, so counts stay numbers.
        if (s.input_tokens)  attrs["llm.token_count.prompt"]     = s.input_tokens;
        if (s.output_tokens) attrs["llm.token_count.completion"] = s.output_tokens;
        if (s.total_tokens)  attrs["llm.token_count.total"]      = s.total_tokens;
        for (const auto& [k, v] : s.attributes) attrs[k] = v;

        json span;
        span["name"]       = s.name;
        span["context"]    = {{"trace_id", s.trace_id}, {"span_id", s.span_id}};
        span["span_kind"]  = to_string(s.kind);
        if (!s.parent_span_id.empty()) span["parent_id"] = s.parent_span_id;
        span["start_time"]     = detail::to_rfc3339(s.start_unix_nano);
        span["end_time"]       = detail::to_rfc3339(s.end_unix_nano);
        span["status_code"]    = detail::phoenix_status(s.status);
        span["status_message"] = s.status_message;
        span["attributes"]     = std::move(attrs);
        span["events"]         = json::array();
        data.push_back(std::move(span));
    }
    return {{"data", std::move(data)}};
}

class PhoenixExporter final : public Exporter {
    PhoenixConfig   cfg_;
    detail::Url     url_;
    httplib::Client client_;

    static std::string build_endpoint(const PhoenixConfig& cfg) {
        auto base = cfg.base_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        auto project = cfg.project_name.empty() ? std::string("default") : cfg.project_name;
        // Phoenix rejects a project name containing '/', '?' or '#'.
        if (project.find_first_of("/?#") != std::string::npos)
            throw Error("phoenix_exporter: project_name must not contain '/', '?' or '#': "
                        + project);
        return base + "/v1/projects/" + project + "/spans";
    }

public:
    explicit PhoenixExporter(PhoenixConfig cfg)
        : cfg_(std::move(cfg))
        , url_(detail::split_url(build_endpoint(cfg_)))
        , client_(url_.origin)
    {
        client_.set_read_timeout(cfg_.timeout_seconds);
        client_.set_write_timeout(cfg_.timeout_seconds);
        if (!cfg_.api_key.empty())
            client_.set_default_headers({{"authorization", "Bearer " + cfg_.api_key}});
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    void export_spans(const std::vector<Span>& spans) override {
        if (spans.empty()) return;
        auto body = to_phoenix_json(spans, cfg_.service_name).dump();
        cfg_.log.debug("tracing", "phoenix: POST " + url_.path + " ("
            + std::to_string(spans.size()) + " span(s))");

        auto res = client_.Post(url_.path, body, "application/json");
        if (!res)
            throw Error("phoenix export failed: " + httplib::to_string(res.error()));
        if (res->status < 200 || res->status >= 300)
            throw Error("phoenix export rejected (status " + std::to_string(res->status)
                + "): " + res->body.substr(0, 512));

        cfg_.log.trace("tracing", "phoenix: " + res->body.substr(0, 200));
    }

    [[nodiscard]] std::string name() const override { return "phoenix"; }
};

inline ExporterPtr phoenix_exporter(PhoenixConfig cfg) {
    return std::make_shared<PhoenixExporter>(std::move(cfg));
}

// For a Phoenix deployment fronted by an OTLP collector that does accept JSON.
// Against Phoenix itself this returns 415 — use phoenix_exporter() instead.
inline OtlpConfig phoenix_otlp_config(PhoenixConfig cfg) {
    auto base = cfg.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();

    OtlpConfig otlp;
    otlp.endpoint = base + "/v1/traces";
    // Lowercase header names: Phoenix documents this so the same config works
    // if you point it at its gRPC collector.
    if (!cfg.api_key.empty())
        otlp.headers["authorization"] = "Bearer " + cfg.api_key;
    if (!cfg.project_name.empty())
        otlp.headers["x-project-name"] = cfg.project_name;
    otlp.service_name    = std::move(cfg.service_name);
    otlp.semconv         = SemConv::openinference;
    otlp.timeout_seconds = cfg.timeout_seconds;
    otlp.label           = "phoenix-otlp";
    otlp.log             = std::move(cfg.log);
    return otlp;
}

} // namespace tiny_agent::obs
