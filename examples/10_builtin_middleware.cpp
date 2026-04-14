#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "guarded",
            .system_prompt = "You are a helpful assistant.",
            .tools = {
                Tool::create("search", "Search the web",
                    [](const json& p) -> json {
                        return "Result for: " + p["query"].get<std::string>();
                    },
                    {{"type", "object"},
                     {"properties", {{"query", {{"type", "string"}}}}},
                     {"required", {"query"}}}),
            },
            .middlewares = {
                // Logging with timing
                middleware::logging(Log{std::cerr, LogLevel::debug}),

                // Retry on transient API errors with exponential backoff
                middleware::model_retry({
                    .max_retries   = 2,
                    .backoff_factor = 2.0,
                    .initial_delay  = 500.0,
                }),

                // Cap total model calls to prevent runaway loops
                middleware::model_call_limit({.limit = 10}),

                // Cap total tool calls
                middleware::tool_call_limit({.limit = 5}),

                // Redact emails from user messages before sending to LLM
                middleware::pii({.pii_type = "email", .strategy = "redact"}),

                // Manage context by clearing old tool results
                middleware::context_editing({.trigger = 50'000, .keep = 5}),

                // Keep conversation manageable
                middleware::trim_history(30),
            },
        },
        Log{std::cerr, LogLevel::debug}
    };

    std::cout << agent.run("Search for 'C++ middleware patterns'") << "\n";
}
