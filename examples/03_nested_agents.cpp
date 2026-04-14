#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    LLMConfig cfg{.api_key = key};

    auto researcher = make_shared_agent(
        LLM<openai>{"gpt-4o-mini", cfg},
        AgentConfig{
            .name = "researcher",
            .system_prompt = "You are a researcher. Provide factual, concise answers.",
        }
    );

    auto writer = make_shared_agent(
        LLM<openai>{"gpt-4o-mini", cfg},
        AgentConfig{
            .name = "writer",
            .system_prompt = "You are a creative writer. Take research notes and produce a short, engaging paragraph.",
        }
    );

    auto manager = make_shared_agent(
        LLM<openai>{"gpt-4o", cfg},
        AgentConfig{
            .name = "manager",
            .system_prompt = "You are a project manager. Delegate research to 'researcher' and writing to 'writer'. "
                             "Combine their outputs into a final answer.",
            .tools = {
                agent_as_tool(researcher, "researcher", "Research factual information about a topic"),
                agent_as_tool(writer, "writer", "Write an engaging paragraph from research notes"),
            },
        },
        Log{std::cerr, LogLevel::debug}
    );

    auto result = manager->run("Write a short paragraph about the James Webb Space Telescope's latest discoveries.");
    std::cout << result << "\n";
}
