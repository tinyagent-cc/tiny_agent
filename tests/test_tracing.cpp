#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/observability/all.hpp>
#include <tiny_agent/tiny_agent.hpp>
#include "mock_model.hpp"
#include <sstream>
#include <cstdlib>
#include <chrono>
#include <thread>

using namespace tiny_agent;
using tiny_agent::test::MockChat;

static std::shared_ptr<obs::MemoryExporter> memory_exporter() {
    return std::make_shared<obs::MemoryExporter>();
}

// ═══════════════════════════════════════════════════════════════════════════
// Ids and clock
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("trace and span ids are the widths OTLP requires") {
    auto t = obs::new_trace_id();
    auto s = obs::new_span_id();
    CHECK(t.size() == 32);
    CHECK(s.size() == 16);
    CHECK(t.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(s.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(obs::new_trace_id() != t);
}

// ═══════════════════════════════════════════════════════════════════════════
// Tracer
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Tracer buffers to max_batch then exports") {
    auto mem = memory_exporter();
    obs::Tracer tracer{mem, {.max_batch = 3}};

    for (int i = 0; i < 2; ++i) tracer.record(obs::Span{});
    CHECK(mem->size() == 0);
    CHECK(tracer.pending() == 2);

    tracer.record(obs::Span{});
    CHECK(mem->size() == 3);
    CHECK(tracer.pending() == 0);
}

TEST_CASE("Tracer flushes what is left") {
    auto mem = memory_exporter();
    {
        obs::Tracer tracer{mem, {.max_batch = 100}};
        tracer.record(obs::Span{});
        CHECK(mem->size() == 0);
    }   // destructor flushes
    CHECK(mem->size() == 1);
}

TEST_CASE("a throwing exporter never reaches the caller") {
    struct Angry final : obs::Exporter {
        int calls = 0;
        void export_spans(const std::vector<obs::Span>&) override {
            ++calls;
            throw std::runtime_error("collector is down");
        }
    };
    auto angry = std::make_shared<Angry>();
    std::ostringstream sink;
    obs::Tracer tracer{angry, {.max_batch = 1, .log = Log{sink, LogLevel::error}}};

    REQUIRE_NOTHROW(tracer.record(obs::Span{}));
    CHECK(angry->calls == 1);
    CHECK(sink.str().find("collector is down") != std::string::npos);
}

TEST_CASE("a default-constructed Tracer discards spans") {
    obs::Tracer tracer;
    REQUIRE_NOTHROW(tracer.record(obs::Span{}));
    CHECK(tracer.exporter()->name() == "noop");
}

// ═══════════════════════════════════════════════════════════════════════════
// ScopedTrace
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("ScopedTrace nests spans under one trace id") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});

    CHECK_FALSE(obs::context::active());
    {
        obs::ScopedTrace outer{tracer, "run"};
        CHECK(obs::context::active());
        {
            obs::ScopedTrace inner{tracer, "step", obs::SpanKind::tool};
            CHECK(obs::context::current_trace_id() == outer.span().trace_id);
        }
    }
    CHECK_FALSE(obs::context::active());

    auto spans = mem->spans();
    REQUIRE(spans.size() == 2);
    // Inner closes first.
    CHECK(spans[0].name == "step");
    CHECK(spans[1].name == "run");
    CHECK(spans[0].trace_id == spans[1].trace_id);
    CHECK(spans[0].parent_span_id == spans[1].span_id);
    CHECK(spans[1].parent_span_id.empty());
    CHECK(spans[1].status == obs::SpanStatus::ok);
}

TEST_CASE("ScopedTrace records an error status") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    {
        obs::ScopedTrace t{tracer, "run"};
        t.set_error("blew up");
    }
    auto spans = mem->spans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].status == obs::SpanStatus::error);
    CHECK(spans[0].status_message == "blew up");
}

// ═══════════════════════════════════════════════════════════════════════════
// OTLP wire format
// ═══════════════════════════════════════════════════════════════════════════

static obs::Span sample_span() {
    obs::Span s;
    s.trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
    s.span_id  = "00f067aa0ba902b7";
    s.name     = "chat";
    s.kind     = obs::SpanKind::llm;
    s.status   = obs::SpanStatus::ok;
    s.model    = "gpt-4o-mini";
    s.system   = "openai";
    s.input    = R"([{"role":"user","content":"hi"}])";
    s.output   = "Hello!";
    s.finish_reason = "stop";
    s.input_tokens  = 12;
    s.output_tokens = 3;
    s.total_tokens  = 15;
    s.start_unix_nano = 1734000000000000000ULL;
    s.end_unix_nano   = 1734000001500000000ULL;
    return s;
}

static const json& find_attr(const json& attrs, const std::string& key) {
    for (const auto& a : attrs)
        if (a.at("key") == key) return a.at("value");
    static const json none = json();
    return none;
}

TEST_CASE("OTLP envelope has the shape the spec defines") {
    auto body = obs::to_otlp_json({sample_span()}, "svc", obs::SemConv::openinference);

    REQUIRE(body.contains("resourceSpans"));
    auto& rs = body["resourceSpans"][0];
    CHECK(find_attr(rs["resource"]["attributes"], "service.name")["stringValue"] == "svc");

    auto& scope = rs["scopeSpans"][0];
    CHECK(scope["scope"]["name"] == "tiny_agent");

    auto& span = scope["spans"][0];
    CHECK(span["traceId"] == "4bf92f3577b34da6a3ce929d0e0e4736");
    CHECK(span["spanId"] == "00f067aa0ba902b7");
    CHECK_FALSE(span.contains("parentSpanId"));       // omitted for a root span
    CHECK(span["kind"] == 3);                         // SPAN_KIND_CLIENT
    CHECK(span["status"]["code"] == 1);               // STATUS_CODE_OK
    // uint64 travels as a string in proto3-JSON.
    CHECK(span["startTimeUnixNano"].is_string());
    CHECK(span["startTimeUnixNano"] == "1734000000000000000");
    CHECK(span["endTimeUnixNano"] == "1734000001500000000");
}

TEST_CASE("OpenInference attributes are what Phoenix reads") {
    auto body = obs::to_otlp_json({sample_span()}, "svc", obs::SemConv::openinference);
    auto& attrs = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0]["attributes"];

    CHECK(find_attr(attrs, "openinference.span.kind")["stringValue"] == "LLM");
    CHECK(find_attr(attrs, "llm.model_name")["stringValue"] == "gpt-4o-mini");
    CHECK(find_attr(attrs, "llm.system")["stringValue"] == "openai");
    CHECK(find_attr(attrs, "input.value")["stringValue"] == R"([{"role":"user","content":"hi"}])");
    CHECK(find_attr(attrs, "output.value")["stringValue"] == "Hello!");
    CHECK(find_attr(attrs, "llm.finish_reason")["stringValue"] == "stop");
    // int64 also travels as a string.
    CHECK(find_attr(attrs, "llm.token_count.prompt")["intValue"] == "12");
    CHECK(find_attr(attrs, "llm.token_count.completion")["intValue"] == "3");
    CHECK(find_attr(attrs, "llm.token_count.total")["intValue"] == "15");
    // The other vocabulary must be absent when it was not requested.
    CHECK(find_attr(attrs, "gen_ai.request.model").is_null());
}

TEST_CASE("OTel GenAI attributes are what Langfuse reads") {
    auto body = obs::to_otlp_json({sample_span()}, "svc", obs::SemConv::otel_genai);
    auto& attrs = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0]["attributes"];

    CHECK(find_attr(attrs, "langfuse.observation.type")["stringValue"] == "generation");
    CHECK(find_attr(attrs, "gen_ai.request.model")["stringValue"] == "gpt-4o-mini");
    CHECK(find_attr(attrs, "gen_ai.system")["stringValue"] == "openai");
    CHECK(find_attr(attrs, "gen_ai.provider.name")["stringValue"] == "openai");
    CHECK(find_attr(attrs, "gen_ai.usage.input_tokens")["intValue"] == "12");
    CHECK(find_attr(attrs, "gen_ai.usage.output_tokens")["intValue"] == "3");
    CHECK(find_attr(attrs, "langfuse.observation.output")["stringValue"] == "Hello!");
    CHECK(find_attr(attrs, "llm.model_name").is_null());
}

TEST_CASE("SemConv::both writes one span both backends can read") {
    auto body = obs::to_otlp_json({sample_span()}, "svc", obs::SemConv::both);
    auto& attrs = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0]["attributes"];
    CHECK(find_attr(attrs, "llm.model_name")["stringValue"] == "gpt-4o-mini");
    CHECK(find_attr(attrs, "gen_ai.request.model")["stringValue"] == "gpt-4o-mini");
}

TEST_CASE("custom span attributes pass through") {
    auto s = sample_span();
    s.attributes["langfuse.session.id"] = "sess-1";
    auto body = obs::to_otlp_json({s}, "svc", obs::SemConv::both);
    auto& attrs = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0]["attributes"];
    CHECK(find_attr(attrs, "langfuse.session.id")["stringValue"] == "sess-1");
}

TEST_CASE("an error span carries STATUS_CODE_ERROR and its message") {
    auto s = sample_span();
    s.status = obs::SpanStatus::error;
    s.status_message = "429 rate limited";
    auto body = obs::to_otlp_json({s}, "svc");
    auto& span = body["resourceSpans"][0]["scopeSpans"][0]["spans"][0];
    CHECK(span["status"]["code"] == 2);
    CHECK(span["status"]["message"] == "429 rate limited");
}

// ═══════════════════════════════════════════════════════════════════════════
// Endpoint and header construction
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("split_url separates origin from path") {
    auto u = obs::detail::split_url("https://cloud.langfuse.com/api/public/otel/v1/traces");
    CHECK(u.origin == "https://cloud.langfuse.com");
    CHECK(u.path == "/api/public/otel/v1/traces");

    auto bare = obs::detail::split_url("http://localhost:6006");
    CHECK(bare.origin == "http://localhost:6006");
    CHECK(bare.path == "/");

    CHECK_THROWS_AS(obs::detail::split_url("localhost:6006"), Error);
}

TEST_CASE("Phoenix span payload matches the shape its API documents") {
    auto body = obs::to_phoenix_json({sample_span()}, "svc");
    REQUIRE(body.contains("data"));
    auto& s = body["data"][0];

    CHECK(s["name"] == "chat");
    CHECK(s["span_kind"] == "LLM");
    CHECK(s["context"]["trace_id"] == "4bf92f3577b34da6a3ce929d0e0e4736");
    CHECK(s["context"]["span_id"] == "00f067aa0ba902b7");
    CHECK_FALSE(s.contains("parent_id"));
    CHECK(s["status_code"] == "OK");
    // RFC 3339 with an explicit zone, which Phoenix requires.
    CHECK(s["start_time"] == "2024-12-12T10:40:00.000000Z");
    CHECK(s["end_time"]   == "2024-12-12T10:40:01.500000Z");

    auto& a = s["attributes"];
    CHECK(a["openinference.span.kind"] == "LLM");
    CHECK(a["llm.model_name"] == "gpt-4o-mini");
    CHECK(a["input.mime_type"] == "application/json");
    // Phoenix takes native JSON numbers here, unlike OTLP's stringified int64.
    CHECK(a["llm.token_count.prompt"] == 12);
    CHECK(a["llm.token_count.total"] == 15);
    CHECK(a["service.name"] == "svc");
}

TEST_CASE("Phoenix error spans and parents survive the mapping") {
    auto s = sample_span();
    s.status = obs::SpanStatus::error;
    s.status_message = "boom";
    s.parent_span_id = "aabbccddeeff0011";
    auto body = obs::to_phoenix_json({s}, "svc");
    CHECK(body["data"][0]["status_code"] == "ERROR");
    CHECK(body["data"][0]["status_message"] == "boom");
    CHECK(body["data"][0]["parent_id"] == "aabbccddeeff0011");
}

TEST_CASE("Phoenix rejects a project name it cannot put in a URL") {
    CHECK_THROWS_AS(obs::phoenix_exporter({.project_name = "a/b"}), Error);
}

TEST_CASE("Phoenix OTLP config keeps the documented headers") {
    auto cfg = obs::phoenix_otlp_config({.base_url = "http://localhost:6006/",
                                         .api_key = "px-key",
                                         .project_name = "my-agent"});
    CHECK(cfg.endpoint == "http://localhost:6006/v1/traces");
    CHECK(cfg.headers.at("authorization") == "Bearer px-key");
    CHECK(cfg.headers.at("x-project-name") == "my-agent");
    CHECK(cfg.semconv == obs::SemConv::openinference);
}

TEST_CASE("rfc3339 formatting is stable at the second and microsecond edges") {
    CHECK(obs::detail::to_rfc3339(0) == "1970-01-01T00:00:00.000000Z");
    CHECK(obs::detail::to_rfc3339(1'000'000'000ULL) == "1970-01-01T00:00:01.000000Z");
    CHECK(obs::detail::to_rfc3339(1'000'999'999ULL) == "1970-01-01T00:00:01.000999Z");
}

TEST_CASE("base64 matches the RFC 4648 vectors") {
    CHECK(obs::detail::base64("") == "");
    CHECK(obs::detail::base64("f") == "Zg==");
    CHECK(obs::detail::base64("fo") == "Zm8=");
    CHECK(obs::detail::base64("foo") == "Zm9v");
    CHECK(obs::detail::base64("foob") == "Zm9vYg==");
    CHECK(obs::detail::base64("fooba") == "Zm9vYmE=");
    CHECK(obs::detail::base64("foobar") == "Zm9vYmFy");
}

TEST_CASE("Langfuse config targets the OTel endpoint with Basic auth") {
    auto cfg = obs::langfuse_otlp_config({.public_key = "pk-lf-xxxx",
                                          .secret_key = "sk-lf-xxxx"});
    CHECK(cfg.endpoint == "https://cloud.langfuse.com/api/public/otel/v1/traces");
    CHECK(cfg.headers.at("Authorization")
          == "Basic " + obs::detail::base64("pk-lf-xxxx:sk-lf-xxxx"));
    CHECK(cfg.headers.at("x-langfuse-ingestion-version") == "4");
    CHECK(cfg.semconv == obs::SemConv::otel_genai);
}

TEST_CASE("Langfuse refuses to build without both keys") {
    CHECK_THROWS_AS(obs::langfuse_otlp_config({.public_key = "pk-only"}), Error);
    CHECK_THROWS_AS(obs::langfuse_otlp_config({.secret_key = "sk-only"}), Error);
}

TEST_CASE("Langfuse stamps trace fields on every span") {
    obs::Span s;
    obs::LangfuseConfig cfg{.public_key = "pk", .secret_key = "sk",
                            .session_id = "sess-9", .user_id = "u-1",
                            .trace_name = "nightly"};
    obs::apply_langfuse_trace_fields(s, cfg);
    CHECK(s.attributes.at("langfuse.session.id") == "sess-9");
    CHECK(s.attributes.at("langfuse.user.id") == "u-1");
    CHECK(s.attributes.at("langfuse.trace.name") == "nightly");
}

// ═══════════════════════════════════════════════════════════════════════════
// StreamExporter
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("StreamExporter prints one line per span and hides content by default") {
    std::ostringstream out;
    obs::StreamExporter exp{out};
    exp.export_spans({sample_span()});

    auto text = out.str();
    CHECK(text.find("LLM chat") != std::string::npos);
    CHECK(text.find("model=gpt-4o-mini") != std::string::npos);
    CHECK(text.find("tokens=15") != std::string::npos);
    CHECK(text.find("Hello!") == std::string::npos);
}

TEST_CASE("StreamExporter shows content when asked") {
    std::ostringstream out;
    obs::StreamExporter exp{out, {.verbose = true}};
    exp.export_spans({sample_span()});
    CHECK(out.str().find("Hello!") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// The middleware
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("tracing middleware records a span per model call") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    auto mw = middleware::tracing({.tracer = tracer, .span_name = "chat", .system = "openai"});

    std::vector<Message> msgs = {Message::user("what is 2+2?")};
    auto resp = mw(msgs, [](std::vector<Message>&) {
        LLMResponse r{Message::assistant("4"), {}, "stop", {}};
        r.usage = {{"prompt_tokens", 7}, {"completion_tokens", 1}, {"total_tokens", 8}};
        r.raw   = {{"model", "gpt-4o-mini"}};
        return r;
    });

    CHECK(resp.message.text() == "4");
    auto spans = mem->spans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].name == "chat");
    CHECK(spans[0].system == "openai");
    CHECK(spans[0].model == "gpt-4o-mini");
    CHECK(spans[0].status == obs::SpanStatus::ok);
    CHECK(spans[0].finish_reason == "stop");
    CHECK(spans[0].input_tokens == 7);
    CHECK(spans[0].output_tokens == 1);
    CHECK(spans[0].total_tokens == 8);
    CHECK(spans[0].output == "4");
    CHECK(spans[0].input.find("what is 2+2?") != std::string::npos);
    CHECK(spans[0].end_unix_nano >= spans[0].start_unix_nano);
}

TEST_CASE("tracing middleware totals tokens when the provider omits the total") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    auto mw = middleware::tracing({.tracer = tracer});

    std::vector<Message> msgs = {Message::user("hi")};
    mw(msgs, [](std::vector<Message>&) {
        LLMResponse r{Message::assistant("yo"), {}, "stop", {}};
        r.usage = {{"input_tokens", 5}, {"output_tokens", 2}};   // Anthropic naming
        return r;
    });

    auto spans = mem->spans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].input_tokens == 5);
    CHECK(spans[0].output_tokens == 2);
    CHECK(spans[0].total_tokens == 7);
}

TEST_CASE("capture_content=false keeps prompts out of the span") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    auto mw = middleware::tracing({.tracer = tracer, .capture_content = false});

    std::vector<Message> msgs = {Message::user("my social security number is 000")};
    mw(msgs, [](std::vector<Message>&) {
        return LLMResponse{Message::assistant("noted"), {}, "stop", {}};
    });

    auto spans = mem->spans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].input.empty());
    CHECK(spans[0].output.empty());
    CHECK(spans[0].status == obs::SpanStatus::ok);   // timings still recorded
}

TEST_CASE("tracing middleware records the failure and rethrows") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    auto mw = middleware::tracing({.tracer = tracer});

    std::vector<Message> msgs = {Message::user("hi")};
    CHECK_THROWS_AS(
        mw(msgs, [](std::vector<Message>&) -> LLMResponse {
            throw APIError(429, "rate limited");
        }),
        APIError);

    auto spans = mem->spans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].status == obs::SpanStatus::error);
    CHECK(spans[0].status_message.find("rate limited") != std::string::npos);
}

TEST_CASE("tracing middleware clips oversized content") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    auto mw = middleware::tracing({.tracer = tracer, .max_content_chars = 10});

    std::vector<Message> msgs = {Message::user(std::string(500, 'x'))};
    mw(msgs, [](std::vector<Message>&) {
        return LLMResponse{Message::assistant(std::string(500, 'y')), {}, "stop", {}};
    });

    auto spans = mem->spans();
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].output.size() < 30);
    CHECK(spans[0].input.size() < 100);
}

TEST_CASE("a traced agent run groups every model call under one trace") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});

    AgentConfig cfg;
    cfg.middlewares.push_back(middleware::tracing({.tracer = tracer, .system = "mock"}));
    cfg.tools.push_back(DynamicTool::create("echo", "echoes",
        [](const json& a) { return a; }));

    MockChat llm;
    llm.script = {MockChat::tool_call("echo", {{"v", 1}}), MockChat::text("done")};

    agents::DeepAgent<MockChat> agent{std::move(llm), std::move(cfg)};

    {
        obs::ScopedTrace run{tracer, "agent-run"};
        CHECK(agent.run("go") == "done");
    }

    auto spans = mem->spans();
    REQUIRE(spans.size() == 3);          // two model calls plus the run
    auto trace_id = spans.back().trace_id;
    for (const auto& s : spans) CHECK(s.trace_id == trace_id);
    CHECK(spans.back().name == "agent-run");
    CHECK(spans.back().kind == obs::SpanKind::agent);
    CHECK(spans[0].parent_span_id == spans.back().span_id);
    CHECK(spans[1].parent_span_id == spans.back().span_id);
}

TEST_CASE("untraced model calls each get their own trace") {
    auto mem = memory_exporter();
    auto tracer = std::make_shared<obs::Tracer>(mem, obs::TracerConfig{.max_batch = 1});
    auto mw = middleware::tracing({.tracer = tracer});

    std::vector<Message> msgs = {Message::user("hi")};
    auto call = [&] {
        mw(msgs, [](std::vector<Message>&) {
            return LLMResponse{Message::assistant("ok"), {}, "stop", {}};
        });
    };
    call();
    call();

    auto spans = mem->spans();
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].trace_id != spans[1].trace_id);
    CHECK(spans[0].parent_span_id.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Live export — skipped unless a collector is configured
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("live OTLP export against a configured collector") {
    const char* endpoint = std::getenv("TINY_AGENT_OTLP_ENDPOINT");
    if (!endpoint) {
        // No collector configured: the offline tests above already cover the
        // payload, so there is nothing to reach here.
        REQUIRE_FALSE(endpoint);
        return;
    }
    auto exporter = obs::otlp_exporter({.endpoint = endpoint,
                                        .service_name = "tiny_agent_test"});
    REQUIRE_NOTHROW(exporter->export_spans({sample_span()}));
}

TEST_CASE("live Phoenix export against a running instance") {
    const char* base = std::getenv("PHOENIX_BASE_URL");
    if (!base) {
        REQUIRE_FALSE(base);
        return;
    }
    auto exporter = obs::phoenix_exporter({.base_url = base,
                                           .api_key = std::getenv("PHOENIX_API_KEY")
                                                ? std::getenv("PHOENIX_API_KEY") : "",
                                           .project_name = "tiny-agent-test",
                                           .service_name = "tiny_agent_test"});
    auto s = sample_span();
    s.trace_id = obs::new_trace_id();
    s.span_id  = obs::new_span_id();
    REQUIRE_NOTHROW(exporter->export_spans({s}));
}

// Reads the ingested observations back out of Langfuse. Ingestion is
// asynchronous (the web tier queues, the worker writes to ClickHouse), so this
// polls until the expected count shows up or the budget runs out.
static json langfuse_read_observations(const std::string& base_url,
                                       const std::string& auth_header,
                                       const std::string& trace_id,
                                       std::size_t expected,
                                       int attempts = 30) {
    auto url  = obs::detail::split_url(base_url + "/api/public/v2/observations");
    httplib::Client client(url.origin);
    client.set_read_timeout(10);
    client.set_default_headers({{"Authorization", auth_header}});
#ifdef __APPLE__
    client.set_ca_cert_path("/etc/ssl/cert.pem");
#endif

    json last = json::object();
    for (int i = 0; i < attempts; ++i) {
        auto res = client.Get(url.path + "?traceId=" + trace_id
                              + "&fields=core,basic,time,io,model,usage,metrics,trace_context");
        if (res && res->status == 200) {
            last = json::parse(res->body, nullptr, false);
            if (!last.is_discarded() && last.contains("data")
                && last["data"].size() >= expected)
                return last;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return last;
}

static const json& observation_named(const json& body, const std::string& name) {
    for (const auto& o : body.at("data"))
        if (o.at("name") == name) return o;
    static const json none = json();
    return none;
}

TEST_CASE("live Langfuse export against configured credentials") {
    const char* pk = std::getenv("LANGFUSE_PUBLIC_KEY");
    const char* sk = std::getenv("LANGFUSE_SECRET_KEY");
    const bool configured = pk != nullptr && sk != nullptr;
    if (!configured) {
        REQUIRE_FALSE(configured);
        return;
    }
    const char* base_env = std::getenv("LANGFUSE_BASE_URL");
    std::string base = base_env ? base_env : "https://cloud.langfuse.com";
    while (!base.empty() && base.back() == '/') base.pop_back();

    obs::LangfuseConfig cfg{.base_url = base,
                            .public_key = pk, .secret_key = sk,
                            .service_name = "tiny_agent_test"};
    cfg.session_id = "tiny-agent-live-session";
    cfg.user_id    = "tiny-agent-live-user";
    cfg.trace_name = "live-roundtrip";
    auto exporter = obs::langfuse_exporter(cfg);

    // A two-span trace: an agent root and a failing model call under it. That
    // covers parenting, the observation-type vocabulary, and the error path,
    // none of which a single healthy span would exercise.
    const auto trace_id = obs::new_trace_id();

    obs::Span root;
    root.trace_id = trace_id;
    root.span_id  = obs::new_span_id();
    root.name     = "live-roundtrip";
    root.kind     = obs::SpanKind::agent;
    root.status   = obs::SpanStatus::ok;
    root.input    = R"({"question":"round trip"})";
    root.output   = "done";
    root.start_unix_nano = obs::now_unix_nano();
    root.end_unix_nano   = root.start_unix_nano + 1'500'000'000ULL;

    auto child = sample_span();
    child.trace_id       = trace_id;
    child.span_id        = obs::new_span_id();
    child.parent_span_id = root.span_id;
    child.name           = "live-chat";
    child.status         = obs::SpanStatus::error;
    child.status_message = "rate limited";
    child.start_unix_nano = root.start_unix_nano;
    child.end_unix_nano   = root.start_unix_nano + 900'000'000ULL;

    REQUIRE_NOTHROW(exporter->export_spans({root, child}));
    MESSAGE("langfuse trace id: " << trace_id);

    // Same Basic header the exporter builds, so the read path proves the
    // credential encoding the write path used.
    auto body = langfuse_read_observations(
        base, obs::langfuse_otlp_config(cfg).headers.at("Authorization"),
        trace_id, 2);
    REQUIRE(body.contains("data"));
    REQUIRE(body["data"].size() == 2);

    const auto& gen = observation_named(body, "live-chat");
    REQUIRE_FALSE(gen.is_null());
    CHECK(gen.at("type")        == "GENERATION");
    CHECK(gen.at("level")       == "ERROR");
    CHECK(gen.at("statusMessage") == "rate limited");
    CHECK(gen.at("model")       == "gpt-4o-mini");
    CHECK(gen.at("input")       == R"([{"role":"user","content":"hi"}])");
    CHECK(gen.at("output")      == "Hello!");
    CHECK(gen.at("usageDetails").at("input")  == 12);
    CHECK(gen.at("usageDetails").at("output") == 3);
    CHECK(gen.at("sessionId")   == "tiny-agent-live-session");
    CHECK(gen.at("userId")      == "tiny-agent-live-user");
    CHECK(gen.at("traceName")   == "live-roundtrip");

    const auto& agent = observation_named(body, "live-roundtrip");
    REQUIRE_FALSE(agent.is_null());
    CHECK(agent.at("type")                == "AGENT");
    CHECK(agent.at("isRootObservation")   == true);
    CHECK(gen.at("parentObservationId")   == agent.at("id"));
}
