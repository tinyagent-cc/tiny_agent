#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::cout << "=== Summarize Middleware Demo ===\n\n";

    // ── Part 1: Extractive summarization (no LLM call) ──────────────────
    //
    // Condenses each older message to a role-prefixed snippet.
    // Tool results are truncated more aggressively since they tend to be
    // large but low-value once consumed.
    {
        std::cout << "--- Part 1: Extractive Summarization (no extra LLM calls) ---\n";

        auto agent = make_agent(
            OpenAIChat{.model="gpt-4o-mini", .api_key=key},
            {
                .name = "extractive",
                .system_prompt = "You are a knowledgeable assistant. Give detailed answers.",
                .middlewares = {
                    middleware::summarize(middleware::SummarizeConfig{
                        .trigger_tokens = 200,
                        .keep_recent    = 2,
                    }),
                },
            }
        );

        const char* topics[] = {
            "Explain how photosynthesis works in detail.",
            "Now explain the water cycle step by step.",
            "Describe the nitrogen cycle and its importance.",
            "How does cellular respiration work?",
        };

        for (auto* q : topics) {
            auto answer = agent.chat(q);
            std::cout << "\n[history=" << agent.history().size() << "] Q: " << q << "\n";
            std::cout << "  A: " << answer.substr(0, 120) << "...\n";

            for (auto& m : agent.history())
                if (m.text().find("[Conversation summary]") != std::string::npos) {
                    std::cout << "  >> SUMMARY ACTIVE: "
                              << m.text().substr(0, 200) << "...\n";
                    break;
                }
        }
    }

    std::cout << "\n";

    // ── Part 2: LLM-backed summarization ────────────────────────────────
    //
    // Uses a separate (cheap) LLM call to produce a coherent summary
    // instead of extractive snippets.
    {
        std::cout << "--- Part 2: LLM-backed Summarization ---\n";

        auto sum_llm = OpenAIChat{.model="gpt-4o-mini", .api_key=key};

        auto llm_fn = [&sum_llm](const std::string& prompt) -> std::string {
            auto resp = sum_llm.chat({Message::user(prompt)});
            return resp.message.text();
        };

        auto agent = make_agent(
            OpenAIChat{.model="gpt-4o-mini", .api_key=key},
            {
                .name = "llm_summarize",
                .system_prompt = "You are a science tutor. Give thorough explanations.",
                .middlewares = {
                    middleware::summarize(middleware::SummarizeConfig{
                        .trigger_tokens = 200,
                        .keep_recent    = 2,
                        .llm_fn         = llm_fn,
                    }),
                },
            }
        );

        const char* topics[] = {
            "What causes earthquakes?",
            "How do volcanoes form?",
            "Explain plate tectonics.",
            "What are seismic waves?",
        };

        for (auto* q : topics) {
            auto answer = agent.chat(q);
            std::cout << "\n[history=" << agent.history().size() << "] Q: " << q << "\n";
            std::cout << "  A: " << answer.substr(0, 120) << "...\n";

            for (auto& m : agent.history())
                if (m.text().find("[Conversation summary]") != std::string::npos) {
                    std::cout << "  >> LLM SUMMARY: "
                              << m.text().substr(0, 250) << "...\n";
                    break;
                }
        }
    }
}
