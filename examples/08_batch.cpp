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
            .name = "batch_agent",
            .system_prompt = "Reply in exactly one sentence.",
        }
    };

    std::vector<std::string> questions = {
        "What is the capital of France?",
        "What is 2 + 2?",
        "Who painted the Mona Lisa?",
        "What is the speed of light?",
    };

    // ── 1. Simple batch — fire-and-collect ──────────────────────────────

    std::cerr << "--- Simple batch ---\n";
    {
        auto results = batch::run(agent, questions);
        for (auto& r : results)
            std::cout << "[" << r.index << "] " << (r.ok() ? *r.output : *r.error) << "\n";

        auto s = batch::summarize(results);
        std::cerr << s.succeeded << "/" << s.total << " succeeded, "
                  << s.failed << " failed, " << s.skipped << " skipped\n";
    }

    // ── 2. Interceptor + retry + all hooks ──────────────────────────────

    std::cerr << "\n--- Batch with hooks ---\n";
    {
        batch::Config cfg{
            .max_retries = 2,
            .retry_delay = std::chrono::milliseconds(300),
        };

        batch::Hooks hooks{
            // Interceptor: transform or skip items before they reach the agent.
            .interceptor = [](std::size_t i, const std::string& input)
                    -> std::optional<std::string> {
                if (input.find("speed") != std::string::npos)
                    return std::nullopt;                       // skip this item
                return "Answer concisely: " + input;           // rewrite prompt
            },

            // On success: log the result size.
            .on_success = [](std::size_t i, const std::string&,
                             const std::string& output) {
                std::cerr << "  [" << i << "] OK (" << output.size() << " chars)\n";
            },

            // On error: decide whether to retry. Returning false short-circuits
            // remaining retries; returning true continues if attempts remain.
            .on_error = [](std::size_t i, const std::string&,
                           const std::exception& ex, int attempt) -> bool {
                std::cerr << "  [" << i << "] attempt " << attempt
                          << " failed: " << ex.what() << "\n";
                return true;
            },

            // On failure: item permanently failed after all retries.
            .on_failure = [](std::size_t i, const std::string&,
                             const std::string& error) {
                std::cerr << "  [" << i << "] PERMANENT FAILURE: " << error << "\n";
            },
        };

        auto results = batch::run(agent, questions, cfg, hooks,
                                  Log{std::cerr, LogLevel::info});
        for (auto& r : results) {
            if (r.ok())
                std::cout << "[" << r.index << "] " << *r.output << "\n";
            else
                std::cout << "[" << r.index << "] ERROR: " << *r.error << "\n";
        }
    }

    // ── 3. Lazy iterator with range-for ─────────────────────────────────

    std::cerr << "\n--- Lazy iterator (range-for) ---\n";
    {
        for (auto& r : batch::iterate(agent, questions)) {
            std::cout << "[" << r.index << "] "
                      << (r.ok() ? *r.output : "ERROR: " + *r.error) << "\n";
        }
    }

    // ── 4. Manual iterator — progress tracking + early stop ─────────────

    std::cerr << "\n--- Manual iterator ---\n";
    {
        auto iter = batch::iterate(agent, questions,
            batch::Config{.stop_on_failure = true});

        while (iter.has_next()) {
            std::cerr << "  processing " << (iter.position() + 1)
                      << "/" << iter.size() << "...\n";
            auto r = iter.next();
            std::cout << "[" << r.index << "] "
                      << (r.ok() ? *r.output : "ERROR: " + *r.error) << "\n";
        }
    }

    // ── 5. Iterator::collect() — drain remaining into a vector ──────────

    std::cerr << "\n--- Partial iterate + collect ---\n";
    {
        auto iter = batch::iterate(agent, questions);

        auto first = iter.next();
        std::cout << "first: " << (first.ok() ? *first.output : *first.error) << "\n";

        auto rest = iter.collect();
        std::cout << "remaining: " << rest.size() << " results\n";
        for (auto& r : rest)
            std::cout << "  [" << r.index << "] " << (r.ok() ? *r.output : *r.error) << "\n";
    }
}
