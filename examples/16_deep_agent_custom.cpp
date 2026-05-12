#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <tiny_agent/providers/anthropic.hpp>
#include <tiny_agent/providers/gemini.hpp>
#include <iostream>
#include <cstdlib>
#include <cmath>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    // ── Single-param create() — llm type deduced from .llm field ──────────
    // create_agent<deep_agent_tag>::create({.llm=..., .name=..., ...})
    // Note: MSVC requires explicit LLM type in create<T>() call.
    // Other compilers may deduce it from the .llm initializer.

    auto agent = create_agent<agents::deep_agent_tag>::create<OpenAIChat>({
        .llm = OpenAIChat{
            .model = "gpt-4o-mini",
            .api_key = key,
            .temperature = 0.7,
            .max_tokens = 500
        },
        .name = "custom_deep",
        .system_prompt = "You are a helpful assistant with tools.",
        .tools = {
            DynamicTool::create("sqrt", "Square root",
                [](const json& p) -> json {
                    return std::sqrt(p["x"].get<double>());
                },
                {{"type", "object"},
                 {"properties", {{"x", {{"type", "number"}}}}},
                 {"required", {"x"}}})
        },
        .max_iterations = 5,
        .llm_config = {.temperature = 0.7, .max_tokens = 500},
        .logger = Log{std::cerr, LogLevel::info},
        .kwargs = {{"note", "single-param create() demo"}}
    });

    auto result = agent.invoke("What is the square root of 144?");
    std::cout << "Result: " << result << "\n";

    // ── Two-param make_agent(llm, config) — the idiomatic shorthand ───────
    // No template params needed — LLM type deduced from first argument.
    // This is the most ergonomic form in C++20.

    auto fast = make_agent(
        OpenAIChat{.model = "gpt-4o-mini", .api_key = key, .temperature = 0.0},
        {.name = "precise", .system_prompt = "Reply with just the number."}
    );
    std::cout << "Precise: " << fast.invoke("What is 6*7?") << "\n";

    // ── init_chat_model with create_agent ─────────────────────────────────
    {
        auto from_init = create_agent<agents::deep_agent_tag>::create<AnyChat>({
            .llm = init_chat_model("openai:gpt-4o-mini", {.api_key = key}),
            .name = "from_init",
            .system_prompt = "Answer concisely."
        });
        std::cout << "init_chat: " << from_init.invoke("Say 'hello'.") << "\n";
    }

    // ── Sub-agent nesting ─────────────────────────────────────────────────
    auto sub = make_shared_agent(
        OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
        {.name = "verifier",
         .system_prompt = "Verify mathematical facts."}
    );

    auto main = make_agent(
        OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
        {.tools = {
            agent_as_tool(sub, "verify", "Verify a claim")
        }}
    );

    std::cout << "Nested: " << main.invoke("Is 144 = 12^2?") << "\n";

    return 0;
}
