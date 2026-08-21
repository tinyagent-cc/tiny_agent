// 18_tracing — send agent traces to a monitoring backend
//
// Tracing is a middleware, so it composes with everything else in the chain and
// costs nothing when it is not in the list. Which backend receives the spans is
// one config struct.
//
//   Default (no setup):   spans print to stderr
//   Arize Phoenix:        PHOENIX_BASE_URL=http://localhost:6006 [PHOENIX_API_KEY=…]
//   Langfuse:             LANGFUSE_PUBLIC_KEY=pk-lf-… LANGFUSE_SECRET_KEY=sk-lf-…
//                         [LANGFUSE_BASE_URL=https://us.cloud.langfuse.com]
//
//   OPENAI_API_KEY=…  ./build/examples/18_tracing
//
// Or point it at a local model instead:
//   OLLAMA_BASE_URL=http://localhost:11434 OLLAMA_MODEL=qwen3 ./build/examples/18_tracing

#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <tiny_agent/providers/local.hpp>
#include <tiny_agent/observability/all.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace tiny_agent;

static std::string env_or(const char* name, std::string fallback = {}) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::move(fallback);
}

// Picks an exporter from whatever is configured, falling back to stderr so the
// example always shows something.
static obs::ExporterPtr pick_exporter() {
    auto lf_public = env_or("LANGFUSE_PUBLIC_KEY");
    auto lf_secret = env_or("LANGFUSE_SECRET_KEY");
    if (!lf_public.empty() && !lf_secret.empty()) {
        std::cout << "exporting to Langfuse\n";
        return obs::langfuse_exporter({
            .base_url   = env_or("LANGFUSE_BASE_URL", "https://cloud.langfuse.com"),
            .public_key = std::move(lf_public),
            .secret_key = std::move(lf_secret),
            .service_name = "tiny_agent_example",
            .trace_name = "tracing-example"});
    }

    auto phoenix_url = env_or("PHOENIX_BASE_URL");
    if (!phoenix_url.empty()) {
        std::cout << "exporting to Phoenix at " << phoenix_url << "\n";
        return obs::phoenix_exporter({
            .base_url = phoenix_url,
            .api_key  = env_or("PHOENIX_API_KEY"),
            .project_name = env_or("PHOENIX_PROJECT", "tiny-agent"),
            .service_name = "tiny_agent_example"});
    }

    std::cout << "no backend configured; printing spans to stderr\n";
    return obs::stderr_exporter({.verbose = true, .max_content_chars = 120});
}

int main() {
    auto tracer = std::make_shared<obs::Tracer>(
        pick_exporter(), obs::TracerConfig{.service_name = "tiny_agent_example"});

    AgentConfig cfg;
    cfg.name = "math_agent";
    cfg.system_prompt = "Use the tools for arithmetic. Answer in one short sentence.";
    cfg.middlewares.push_back(middleware::tracing({
        .tracer = tracer,
        .span_name = "chat",
        .system = "openai",
        .attributes = {{"example", "18_tracing"}}}));
    cfg.tools.push_back(DynamicTool::create("sqrt", "Square root of a number",
        [](const json& args) -> json { return std::sqrt(args.at("x").get<double>()); },
        {{"type", "object"},
         {"properties", {{"x", {{"type", "number"}}}}},
         {"required", {"x"}}}));

    auto base_url = env_or("OLLAMA_BASE_URL");
    auto key      = env_or("OPENAI_API_KEY");
    if (base_url.empty() && key.empty()) {
        std::cerr << "set OPENAI_API_KEY, or OLLAMA_BASE_URL for a local model\n";
        return 1;
    }

    auto run = [&](auto llm) {
        auto agent = make_agent(std::move(llm), std::move(cfg));
        // The ScopedTrace is what turns three separate model calls into one
        // trace with a run span over them. Drop it and each call stands alone.
        obs::ScopedTrace run_span{tracer, "sqrt-question"};
        try {
            auto answer = agent.run("What is the square root of 1764?");
            run_span.span().output = answer;
            std::cout << "\n" << answer << "\n";
        } catch (const std::exception& e) {
            run_span.set_error(e.what());
            throw;
        }
    };

    try {
        if (!base_url.empty())
            run(local::ollama(env_or("OLLAMA_MODEL", "qwen3"),
                              LLMConfig{.base_url = base_url}));
        else
            run(OpenAIChat{.model = "gpt-4o-mini", .api_key = key});
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        tracer->flush();
        return 1;
    }

    // Anything still buffered goes out here; the destructor would do it too.
    tracer->flush();
}
