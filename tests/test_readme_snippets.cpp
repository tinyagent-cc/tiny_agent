#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
// Every code block in README.md, verbatim. Compiling is the test: it fails the
// build the moment the README documents an API that no longer exists, which is
// how the previous README ended up showing an Agent type and a Tool::create
// that were never in this codebase.
//
// None of these functions is ever called — they would all hit the network.
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <tiny_agent/providers/anthropic.hpp>
#include <tiny_agent/providers/local.hpp>
#include <tiny_agent/init_chat_model.hpp>
#include <tiny_agent/observability/all.hpp>
#include <cmath>
#include <iostream>

using namespace tiny_agent;

void deep_agents(const char* key, const char* anthropic_key) {
    auto fact_checker = make_shared_agent(
        OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
        AgentConfig{.name = "fact_checker",
                    .system_prompt = "Reply VERIFIED or UNVERIFIED with one line of why."});

    auto analyst = make_shared_agent(
        AnthropicChat{.model = "claude-sonnet-4-5", .api_key = anthropic_key},
        AgentConfig{
            .name = "analyst",
            .system_prompt = "Analyze the topic. Verify every claim with fact_checker.",
            .tools = {agent_as_tool(fact_checker, "fact_checker", "Verify a claim")}});

    auto director = make_shared_agent(
        OpenAIChat{.model = "gpt-4o", .api_key = key},
        AgentConfig{
            .name = "director",
            .tools = {agent_as_tool(analyst, "analyst", "Analyze a topic")},
            .logger = Log{std::cerr, LogLevel::debug}});

    std::cout << director->run("What did renewables do to global CO2 emissions?");
}

void middleware_stack(std::shared_ptr<obs::Tracer> tracer) {
    AgentConfig cfg;
    cfg.middlewares = {
        middleware::logging(Log{std::cerr, LogLevel::debug}),
        middleware::tracing({.tracer = tracer}),
        middleware::context_management({.budget = {.max_tokens = 8000, .keep_recent = 6}}),
        middleware::model_retry({.max_retries = 3}),
    };
}

#ifdef TINY_AGENT_HAS_RETE
// Only compiled when RETE_INCLUDE_DIR is set (see tests/CMakeLists.txt): the
// reflex snippet is optional like the rest of rete_cpp, so it can't be a hard
// dependency of the always-on offline suite the way the rest of this file is.
#include <tiny_agent/middleware/reflex.hpp>

void reflex_snippet() {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("ping")
        .when("msg", "text", "ping")
        .then([&](auto&, auto&) { rx.outcome().respond("pong"); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({.extract_facts = middleware::message_facts}));
}
#endif

void chat(const char* key) {
    auto llm = OpenAIChat{.model = "gpt-4o-mini", .api_key = key};
    auto response = llm.chat({Message::system("Be concise."),
                              Message::user("What is the capital of Japan?")});
    std::cout << response.message.text() << "\n";
}

void tool_agent(const char* key) {
    auto agent = make_agent(
        OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
        AgentConfig{
            .name = "math_agent",
            .system_prompt = "Use tools for calculations.",
            .tools = {
                DynamicTool::create("sqrt", "Square root of a number",
                    [](const json& p) -> json { return std::sqrt(p["x"].get<double>()); },
                    {{"type", "object"},
                     {"properties", {{"x", {{"type", "number"}}}}},
                     {"required", {"x"}}})
            },
            .logger = Log{std::cerr, LogLevel::info}});

    std::cout << agent.run("What is sqrt(144)?") << "\n";
}

void local_agent() {
    auto agent = make_agent(local::ollama("llama3"),
                            AgentConfig{.system_prompt = "Reply briefly."});
    std::cout << agent.run("One sentence about C++20.") << "\n";
}

void logging_snippet(OpenAIChat llm) {
    auto agent = make_agent(std::move(llm), AgentConfig{.name = "my_agent",
                                             .logger = Log{std::cerr, LogLevel::debug}});
    agent.log().set_level(LogLevel::trace);
}

void runtime_provider() {
    auto any = init_chat_model("openai:gpt-4o");
    (void)any;
    auto other = local::create("m", "http://localhost:9999");
    (void)other;
    auto stack = make_middleware_stack(middleware::TrimHistory<8>{});
    (void)stack;
}

TEST_CASE("README code blocks still compile") {
    // Reaching this test at all means every snippet above type-checked.
    CHECK(true);
}
