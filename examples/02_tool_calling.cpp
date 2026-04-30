#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <cmath>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    auto agent = AgentExecutor{
        OpenAIChat{"gpt-4o-mini", key},
        AgentConfig{
            .name = "math_agent",
            .system_prompt = "You are a math assistant. Use the provided tools to compute answers.",
            .tools = {
                DynamicTool::create("add", "Add two numbers",
                    [](const json& p) -> json {
                        return p["a"].get<double>() + p["b"].get<double>();
                    },
                    {{"type", "object"},
                     {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
                     {"required", {"a", "b"}}}),

                DynamicTool::create("sqrt", "Square root of a number",
                    [](const json& p) -> json {
                        return std::sqrt(p["x"].get<double>());
                    },
                    {{"type", "object"},
                     {"properties", {{"x", {{"type", "number"}}}}},
                     {"required", {"x"}}}),
            },
        },
        Log{std::cerr, LogLevel::debug}
    };

    auto answer = agent.run("What is sqrt(144) + sqrt(256)?");
    std::cout << "Answer: " << answer << "\n";
}
