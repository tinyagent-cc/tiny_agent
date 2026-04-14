#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    // No .system_prompt in AgentConfig — the middleware injects one.
    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "pirate",
            .middlewares = {
                middleware::system_prompt(
                    "You are a pirate captain named Blackbeard. Always respond in "
                    "pirate speak. Keep answers under two sentences. End with 'Arrr!'"),
            },
        },
    };

    std::cout << "=== System Prompt Middleware Demo ===\n"
              << "(No system_prompt in AgentConfig — middleware injects one)\n\n";

    struct QA { const char* q; };
    QA questions[] = {
        {"What is the weather like today?"},
        {"How do I bake a cake?"},
        {"Explain quantum computing."},
    };

    for (auto& [q] : questions) {
        std::cout << "Q: " << q << "\n"
                  << "A: " << agent.run(q) << "\n\n";
    }
}
