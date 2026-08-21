# Observability

Tracing in tiny_agent is a middleware. It sits in the same chain as retry and
summarize, it wraps the model call, and if you leave it out of the list nothing
about the agent changes. There is no global tracer, no init call, and no
provider registry.

Everything here lives under `include/tiny_agent/observability/` and is opt-in:
`tiny_agent.hpp` does not include any of it, and none of it adds a dependency
beyond the `nlohmann::json` and `httplib` already in the build.

## The pieces

| Type | Where | What it is |
|---|---|---|
| `obs::Span` | `observability/trace.hpp` | One timed operation, with the GenAI fields a model call produces |
| `obs::Exporter` | `observability/trace.hpp` | One method: receive a batch of spans. Implement it to reach any backend |
| `obs::Tracer` | `observability/trace.hpp` | Buffers spans and hands them to an exporter. Thread-safe |
| `obs::ScopedTrace` | `observability/trace.hpp` | RAII run boundary that groups a run's model calls into one trace |
| `middleware::tracing` | `middleware/tracing.hpp` | The middleware that turns each model call into a span |

## Minimal setup

```cpp
#include <tiny_agent/observability/all.hpp>

auto tracer = std::make_shared<obs::Tracer>(obs::stderr_exporter());

AgentConfig cfg;
cfg.middlewares.push_back(middleware::tracing({.tracer = tracer}));

auto agent = make_agent(OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
                        std::move(cfg));
agent.run("What is 12 squared?");
```

Each model call prints a line:

```
[trace] 4bf92f35/00f067aa LLM chat 412.7ms status=ok model=gpt-4o-mini tokens=87
```

## Grouping a run into one trace

The middleware sees one model call at a time and has no way to know where a run
begins. `ScopedTrace` supplies that boundary. While one is alive, every span the
middleware creates joins its trace as a child, and the `ScopedTrace` itself is
emitted as the parent span:

```cpp
{
    obs::ScopedTrace run{tracer, "research-question"};
    auto answer = agent.run("...");
    run.span().output = answer;
}   // the run span closes and records here
```

Without a `ScopedTrace`, each model call is its own single-span trace. That is
still useful — timings, tokens, errors — but the tool-calling turns of one run
are no longer visibly related.

The context is `thread_local`, which matches the rule that an agent instance
belongs to one thread.

## Exporters

### Built in, no backend needed

```cpp
obs::stderr_exporter({.verbose = true});   // one line per span, optionally with content
obs::noop_exporter();                      // discards everything (the default)
obs::MemoryExporter{};                     // keeps spans in a vector; what the tests use
```

A default-constructed `obs::Tracer` uses the no-op exporter, so tracing code
stays compiled and measurable with nothing configured and nothing emitted.

### Arize Phoenix

```cpp
auto tracer = std::make_shared<obs::Tracer>(
    obs::phoenix_exporter({.base_url = "http://localhost:6006",
                           .project_name = "my-agent"}));
```

Spans go to `POST /v1/projects/{project}/spans` as JSON, carrying OpenInference
attributes (`openinference.span.kind`, `llm.model_name`, `input.value`,
`llm.token_count.*`).

**Why not Phoenix's OTLP endpoint.** Phoenix's collector at `/v1/traces` accepts
protobuf only — posting an OTLP/HTTP body with `Content-Type: application/json`
returns `415 Unsupported content type: application/json`, verified against
arize-phoenix on 2026-08-21. Encoding protobuf by hand to send a trace is not a
trade tiny_agent will make, and Phoenix's own span API is JSON and lands spans
in the same project view. `phoenix_otlp_config()` is still there for a Phoenix
deployment fronted by a collector that does accept JSON.

Set `api_key` when the deployment has auth on; a local `phoenix serve` does not
need it.

### Langfuse

```cpp
auto tracer = std::make_shared<obs::Tracer>(
    obs::langfuse_exporter({.public_key = "pk-lf-…",
                            .secret_key = "sk-lf-…",
                            .session_id = "session-42"}));
```

Spans go to `POST /api/public/otel/v1/traces` as OTLP/HTTP JSON with Basic auth
over the key pair, carrying OTel GenAI and Langfuse attributes
(`gen_ai.request.model`, `gen_ai.usage.*`, `langfuse.observation.type`).

**Why the OTel endpoint and not the native ingestion API.** Langfuse's
`/api/public/ingestion` batch API is deprecated: Cloud sunsets it on 2026-11-16
and self-hosted v4 disables it under the default write mode. Shipping a second
wire format with a known expiry date is not worth the maintenance, so tiny_agent
implements the path Langfuse itself recommends.

Default `base_url` is Cloud EU. Use `https://us.cloud.langfuse.com` for US, or
your own URL for self-hosted (needs v3.22.0 or newer for the OTel endpoint).

`session_id`, `user_id` and `trace_name` are stamped onto every span rather than
just the root, because Langfuse reads trace-level fields off whichever span it
is processing.

### Any OTLP collector

```cpp
obs::otlp_exporter({.endpoint = "http://localhost:4318/v1/traces",
                    .semconv  = obs::SemConv::both});
```

`SemConv` picks the attribute vocabulary: `openinference` for Phoenix-style
tooling, `otel_genai` for Langfuse and OTel-native tooling, or `both` (the
default) to write each span twice under both sets of names. Backends ignore
attributes they do not recognize, so `both` gives one span shape that reads
correctly everywhere at the cost of a larger payload.

### Writing your own

Implement one method:

```cpp
struct MyExporter final : obs::Exporter {
    void export_spans(const std::vector<obs::Span>& spans) override {
        for (const auto& s : spans) { /* … */ }
    }
    std::string name() const override { return "mine"; }
};

auto tracer = std::make_shared<obs::Tracer>(std::make_shared<MyExporter>());
```

Or skip the inheritance — anything with a matching `export_spans` satisfies the
`obs::trace_exporter` concept and `obs::make_exporter()` wraps it.

## Failure behaviour

An exporter that throws never reaches the caller. The tracer catches, logs the
failure at `error` level with the span count, and drops the batch. Losing traces
when a collector is down is the correct outcome; losing the agent run is not.

To see those failures, give the tracer a log:

```cpp
obs::Tracer tracer{exporter, {.log = Log{std::cerr, LogLevel::error}}};
```

## Batching and flushing

Spans buffer until `max_batch` (32 by default), then go out in one request. Set
it to 1 to export each span as it closes, which is what you want while
debugging. `flush()` sends whatever is pending; the destructor calls it too, so
a tracer that goes out of scope at the end of `main` does not lose the tail.

## Keeping content out

By default a span carries the prompt and the completion. Turn that off when the
conversation contains anything that should not reach a third-party system:

```cpp
middleware::tracing({.tracer = tracer, .capture_content = false});
```

Timings, model name, token counts, finish reason and errors still flow — only
the message content is withheld. `max_content_chars` (8192 by default) caps what
is sent when capture is on.

## What a span carries

`obs::Span` fields the middleware fills in from an `LLMResponse`:

- `model` — read from the provider's echoed `model` in the raw response body
- `input_tokens` / `output_tokens` / `total_tokens` — read from `usage`,
  accepting OpenAI (`prompt_tokens`), Anthropic (`input_tokens`) and Gemini
  (`promptTokenCount`) naming. If the provider reports no total, it is summed
- `finish_reason`, `status`, `status_message`
- `input` / `output` — the serialized messages and completion, when capture is on

Anything else goes in `attributes`, a plain string map passed through to the
backend verbatim. Set them per-middleware:

```cpp
middleware::tracing({.tracer = tracer,
                     .attributes = {{"deployment", "pi5"}, {"env", "prod"}}});
```

## Example

`examples/18_tracing.cpp` picks a backend from the environment — Langfuse if the
key pair is set, Phoenix if `PHOENIX_BASE_URL` is, stderr otherwise — and runs a
tool-calling agent inside a `ScopedTrace`.

```bash
OPENAI_API_KEY=… ./build/examples/18_tracing
PHOENIX_BASE_URL=http://localhost:6006 OPENAI_API_KEY=… ./build/examples/18_tracing
```
