#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  middleware/tracing.hpp  —  one span per model call
//
//  Tracing is a middleware like any other, which means it sees exactly one model
//  call at a time. To group the calls of a single agent run into one trace, open
//  an obs::ScopedTrace around the run:
//
//    auto tracer = std::make_shared<obs::Tracer>(obs::stderr_exporter());
//    AgentConfig cfg;
//    cfg.middlewares.push_back(middleware::tracing({.tracer = tracer}));
//    ...
//    {
//        obs::ScopedTrace run{tracer, "research"};
//        agent.run("...");
//    }                                  // the run span closes and flushes
//
//  Without a ScopedTrace each model call is a standalone one-span trace.
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/middleware.hpp"
#include "../observability/trace.hpp"

namespace tiny_agent::middleware {

struct TracingConfig {
    obs::TracerPtr tracer;
    std::string    span_name = "chat";
    std::string    system;              // "openai", "anthropic", … if you know it
    // Whether prompts and completions go to the backend. Turn it off when the
    // conversation carries anything you would not put in a third-party system;
    // timings, token counts and errors still flow.
    bool           capture_content   = true;
    std::size_t    max_content_chars = 8192;
    // Extra attributes stamped on every span this middleware creates.
    std::map<std::string, std::string> attributes;
};

namespace detail {

inline std::string clip(std::string s, std::size_t max) {
    if (s.size() > max) { s.resize(max); s += "…"; }
    return s;
}

// The message vector as the model sees it, minus the parts a trace UI cannot
// render usefully.
inline std::string serialize_messages(const std::vector<Message>& msgs, std::size_t max) {
    json arr = json::array();
    for (const auto& m : msgs) {
        json j;
        j["role"] = to_string(m.role);
        auto text = m.text();
        if (!text.empty()) j["content"] = clip(std::move(text), max);
        if (m.has_tool_calls()) {
            json calls = json::array();
            for (const auto& tc : m.tool_calls)
                calls.push_back({{"name", tc.name}, {"arguments", tc.arguments}});
            j["tool_calls"] = std::move(calls);
        }
        if (m.name) j["name"] = *m.name;
        arr.push_back(std::move(j));
    }
    return arr.dump();
}

inline std::int64_t usage_field(const json& usage,
                                std::initializer_list<const char*> keys) {
    for (const auto* k : keys)
        if (usage.contains(k) && usage[k].is_number_integer())
            return usage[k].get<std::int64_t>();
    return 0;
}

} // namespace detail

// Wraps the rest of the chain in a span. On success the span carries the model,
// token usage, finish reason and (optionally) the prompt and completion; on an
// exception it carries the error and rethrows, so tracing never changes what the
// caller sees.
inline MiddlewareFn tracing(TracingConfig cfg) {
    if (!cfg.tracer) cfg.tracer = std::make_shared<obs::Tracer>();

    return [cfg = std::move(cfg)](std::vector<Message>& msgs, Next next) -> LLMResponse {
        obs::Span span;
        span.trace_id = obs::context::active()
            ? obs::context::current_trace_id() : obs::new_trace_id();
        span.parent_span_id  = obs::context::current_span_id();
        span.span_id         = obs::new_span_id();
        span.name            = cfg.span_name;
        span.kind            = obs::SpanKind::llm;
        span.system          = cfg.system;
        span.attributes      = cfg.attributes;
        span.start_unix_nano = obs::now_unix_nano();
        if (cfg.capture_content)
            span.input = detail::serialize_messages(msgs, cfg.max_content_chars);

        try {
            auto resp = next(msgs);

            span.end_unix_nano = obs::now_unix_nano();
            span.status        = obs::SpanStatus::ok;
            span.finish_reason = resp.finish_reason;

            // The model name is not on LLMResponse; every provider echoes it in
            // the raw body, so read it from there when it is a parsed object.
            if (resp.raw.is_object() && resp.raw.contains("model")
                && resp.raw["model"].is_string())
                span.model = resp.raw["model"].get<std::string>();

            if (resp.usage.is_object()) {
                span.input_tokens = detail::usage_field(resp.usage,
                    {"prompt_tokens", "input_tokens", "promptTokenCount"});
                span.output_tokens = detail::usage_field(resp.usage,
                    {"completion_tokens", "output_tokens", "candidatesTokenCount"});
                span.total_tokens = detail::usage_field(resp.usage,
                    {"total_tokens", "totalTokenCount"});
                if (!span.total_tokens)
                    span.total_tokens = span.input_tokens + span.output_tokens;
            }

            if (cfg.capture_content) {
                auto text = resp.message.text();
                if (resp.message.has_tool_calls()) {
                    json calls = json::array();
                    for (const auto& tc : resp.message.tool_calls)
                        calls.push_back({{"name", tc.name}, {"arguments", tc.arguments}});
                    span.output = json{{"content", text}, {"tool_calls", calls}}.dump();
                    span.output_mime_type = "application/json";
                } else {
                    span.output = detail::clip(std::move(text), cfg.max_content_chars);
                }
            }

            cfg.tracer->record(std::move(span));
            return resp;
        } catch (const std::exception& e) {
            span.end_unix_nano   = obs::now_unix_nano();
            span.status          = obs::SpanStatus::error;
            span.status_message  = e.what();
            cfg.tracer->record(std::move(span));
            throw;
        } catch (...) {
            span.end_unix_nano   = obs::now_unix_nano();
            span.status          = obs::SpanStatus::error;
            span.status_message  = "unknown non-standard exception";
            cfg.tracer->record(std::move(span));
            throw;
        }
    };
}

} // namespace tiny_agent::middleware
