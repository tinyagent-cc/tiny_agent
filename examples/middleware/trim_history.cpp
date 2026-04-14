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
            .name = "geography",
            .system_prompt = "You are a geography expert. Reply in exactly one sentence.",
            .middlewares = {
                // Keep system prompt + at most 4 non-system messages.
                // Older messages are silently dropped.
                middleware::trim_history(4),
            },
        }
    };

    std::cout << "=== Trim History Middleware Demo ===\n"
              << "(max_messages=4: system prompt + 4 most recent messages kept)\n\n";

    const char* questions[] = {
        "What is the capital of France?",
        "What about Germany?",
        "And Japan?",
        "What about Brazil?",
        "And Australia?",
        "Now tell me about Canada.",
        "What about Egypt?",
        "And South Korea?",
    };

    for (auto* q : questions) {
        auto answer = agent.chat(q);
        std::cout << "[history=" << agent.history().size() << "] "
                  << "Q: " << q << "\n"
                  << "  A: " << answer << "\n";
    }

    std::cout << "\n--- Final history (" << agent.history().size() << " messages) ---\n";
    for (auto& m : agent.history())
        std::cout << "  " << to_string(m.role) << ": "
                  << m.text().substr(0, 80)
                  << (m.text().size() > 80 ? "..." : "") << "\n";
}
