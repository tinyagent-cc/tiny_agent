#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <memory>

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

        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "extractive",
                .system_prompt = "You are a knowledgeable assistant. Give detailed answers.",
                .middlewares = {
                    middleware::summarize(middleware::SummarizeConfig{
                        .trigger_tokens = 200,
                        .keep_recent    = 2,
                    }),
                },
            }
        };

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

        auto sum_llm = std::make_shared<OpenAIChat>(
            std::string("gpt-4o-mini"), LLMConfig{.api_key = key});

        middleware::SummarizerFn llm_summarizer =
            [sum_llm](const std::vector<Message>& msgs) -> std::string {
                std::string combined;
                for (auto& m : msgs) {
                    auto t = m.text();
                    if (t.empty()) continue;
                    combined += std::string(to_string(m.role)) + ": "
                             + t.substr(0, 300) + "\n";
                }
                auto resp = sum_llm->chat({
                    Message::system("Condense this conversation into 2-3 key points. "
                                    "Be very brief — under 100 words."),
                    Message::user(combined)
                });
                return resp.message.text();
            };

        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "llm_summarize",
                .system_prompt = "You are a science tutor. Give thorough explanations.",
                .middlewares = {
                    middleware::summarize(middleware::SummarizeConfig{
                        .trigger_tokens = 200,
                        .keep_recent    = 2,
                        .summarizer     = llm_summarizer,
                    }),
                },
            }
        };

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
