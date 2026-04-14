#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::cout << "=== Call Limits Middleware Demo ===\n\n";

    auto add_tool = Tool::create("add", "Add two numbers",
        [](const json& p) -> json {
            double a = p["a"].get<double>(), b = p["b"].get<double>();
            std::cout << "  [tool] add(" << a << ", " << b << ") = " << (a + b) << "\n";
            return a + b;
        },
        {{"type", "object"},
         {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
         {"required", {"a", "b"}}});

    // ── 1. Model call limit ─────────────────────────────────────────────
    //
    // Caps total LLM calls per run(). With run_limit=3, the agent gets
    // 3 LLM calls then receives a graceful stop message.
    {
        std::cout << "--- Model Call Limit (run_limit=3) ---\n";

        auto agent = Agent{
            LLM<openai>{"gpt-4o-mini", key},
            AgentConfig{
                .name = "limited",
                .system_prompt = "You are a math assistant. Perform exactly ONE addition per step. "
                                 "Never batch multiple operations.",
                .tools = {add_tool},
                .middlewares = {
                    middleware::model_call_limit({.run_limit = 3}),
                },
                .max_iterations = 10,
            },
            Log{std::cerr, LogLevel::info}
        };

        auto result = agent.run(
            "Compute step by step: ((1+2)+3)+4)+5. Do one add() per step.");
        std::cout << "  Result: " << result << "\n\n";
    }

    // ── 2. Tool call limit ──────────────────────────────────────────────
    //
    // Caps total tool calls across all LLM responses. With run_limit=2
    // and exit_behavior="end", tool calls beyond the limit are stripped
    // from the response, ending the loop.
    {
        std::cout << "--- Tool Call Limit (run_limit=2, exit_behavior=end) ---\n";

        auto agent = Agent{
            LLM<openai>{"gpt-4o-mini", key},
            AgentConfig{
                .name = "tool_limited",
                .system_prompt = "You are a math assistant. Perform one addition per step.",
                .tools = {add_tool},
                .middlewares = {
                    middleware::tool_call_limit({
                        .run_limit = 2,
                        .exit_behavior = "end",
                    }),
                },
                .max_iterations = 10,
            },
            Log{std::cerr, LogLevel::info}
        };

        auto result = agent.run(
            "Compute step by step: (1+2), then (3+4), then (5+6), then (7+8). "
            "One add() per step.");
        std::cout << "  Result: " << result << "\n\n";
    }

    // ── 3. Tool call limit — per-tool targeting ─────────────────────────
    //
    // Only counts calls to a specific tool, leaving others unlimited.
    {
        std::cout << "--- Tool Call Limit (per-tool: max 1 'multiply') ---\n";

        auto multiply_tool = Tool::create("multiply", "Multiply two numbers",
            [](const json& p) -> json {
                double a = p["a"].get<double>(), b = p["b"].get<double>();
                std::cout << "  [tool] multiply(" << a << ", " << b << ") = " << (a * b) << "\n";
                return a * b;
            },
            {{"type", "object"},
             {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
             {"required", {"a", "b"}}});

        auto agent = Agent{
            LLM<openai>{"gpt-4o-mini", key},
            AgentConfig{
                .name = "per_tool",
                .system_prompt = "You are a math assistant. Perform one operation per step.",
                .tools = {add_tool, multiply_tool},
                .middlewares = {
                    middleware::tool_call_limit({
                        .run_limit = 1,
                        .tool_name = "multiply",
                        .exit_behavior = "end",
                    }),
                },
                .max_iterations = 10,
            },
            Log{std::cerr, LogLevel::info}
        };

        auto result = agent.run(
            "Compute: add(2,3), then multiply the result by 4, then multiply by 5. "
            "One operation per step.");
        std::cout << "  Result: " << result << "\n";
    }
}
