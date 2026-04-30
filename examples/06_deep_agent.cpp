#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    LLMConfig cfg{.api_key = key};

    auto fact_checker = make_shared_agent(
        OpenAIChat{"gpt-4o-mini", cfg},
        AgentConfig{
            .name = "fact_checker",
            .system_prompt = "You verify claims. Reply with VERIFIED or UNVERIFIED followed by a one-line explanation.",
        }
    );

    auto analyst = make_shared_agent(
        OpenAIChat{"gpt-4o-mini", cfg},
        AgentConfig{
            .name = "analyst",
            .system_prompt = "You analyze topics and produce bullet-point summaries. "
                             "Use fact_checker to verify any claims you make.",
            .tools = { agent_as_tool(fact_checker, "fact_checker", "Verify a factual claim") },
        }
    );

    auto director = make_shared_agent(
        OpenAIChat{"gpt-4o", cfg},
        AgentConfig{
            .name = "director",
            .system_prompt = "You are the director. Use the analyst to produce a verified analysis. "
                             "Present the final result clearly.",
            .tools = { agent_as_tool(analyst, "analyst", "Analyze a topic with fact-checking") },
        },
        Log{std::cerr, LogLevel::debug}
    );

    auto result = director->run("Analyze the impact of renewable energy on global CO2 emissions.");
    std::cout << result << "\n";
}
