#include <tiny_agent/init_chat_model.hpp>
#include <tiny_agent/agent.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    // Create an LLM from a provider:model string — no compile-time provider tag needed.
    auto llm = init_chat_model("openai:gpt-4o-mini", LLMConfig{.api_key = key});

    // Works with AgentExecutor<deep_agent_tag, AnyChat> seamlessly.
    auto agent = AgentExecutor{std::move(llm), AgentConfig{
        .name = "dynamic_agent",
        .system_prompt = "You are a concise assistant.",
    }};

    std::cout << agent.run("Explain init_chat_model in one sentence.") << "\n";

    // Auto-detected provider (prefix heuristic):
    auto llm2 = init_chat_model("gpt-4o-mini", LLMConfig{.api_key = key});
    std::cout << "model_name: " << llm2.model_name() << "\n";
}
