#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/init_chat_model.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::cout << "=== Retry & Fallback Middleware Demo ===\n\n";
    Log log{std::cerr, LogLevel::info};

    // ── 1. Basic retry (template version) ───────────────────────────────
    //
    // Retries on APIError with status >= 500 (server errors only).
    // Uses exponential backoff: 500ms, 1000ms, 2000ms.
    {
        std::cout << "--- Retry Middleware (configured, triggers on 5xx errors) ---\n";

        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "retry_demo",
                .system_prompt = "Reply in one sentence.",
                .middlewares = {
                    middleware::retry(3, std::chrono::milliseconds(500), log),
                },
            },
            log
        };

        std::cout << "  " << agent.run("What is 2+2?") << "\n\n";
    }

    // ── 2. Model retry (enhanced) ───────────────────────────────────────
    //
    // Retries on ANY APIError with configurable exponential backoff,
    // jitter, and failure modes.
    {
        std::cout << "--- Model Retry (enhanced, with backoff config) ---\n";

        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "model_retry_demo",
                .system_prompt = "Reply in one sentence.",
                .middlewares = {
                    middleware::model_retry({
                        .max_retries    = 2,
                        .backoff_factor = 2.0,
                        .initial_delay  = 500.0,
                        .max_delay      = 5000.0,
                        .jitter         = true,
                        .on_failure     = "continue",
                    }),
                },
            },
            log
        };

        std::cout << "  " << agent.run("What is the speed of light?") << "\n\n";
    }

    // ── 3. Model fallback — actually triggered ──────────────────────────
    //
    // Primary model uses a deliberately invalid name → OpenAI returns
    // an error. The middleware catches it and falls back to a valid model.
    {
        std::cout << "--- Model Fallback (invalid primary → valid fallback) ---\n";

        std::vector<AnyChat> fallbacks;
        fallbacks.push_back(
            init_chat_model("openai:gpt-4o-mini", LLMConfig{.api_key = key}));

        auto agent = AgentExecutor{
            OpenAIChat{"nonexistent-model-xyz", key},
            AgentConfig{
                .name = "fallback_demo",
                .system_prompt = "Reply in one sentence.",
                .middlewares = {
                    middleware::model_fallback(std::move(fallbacks), {}, log),
                },
            },
            log
        };

        std::cout << "  Primary model 'nonexistent-model-xyz' will fail...\n";
        auto result = agent.run("What is the capital of France?");
        std::cout << "  Fallback succeeded: " << result << "\n\n";
    }

    // ── 4. Combined: retry + fallback ───────────────────────────────────
    //
    // model_retry wraps model_fallback. If the primary fails, fallback
    // handles it. If all fallbacks fail too, model_retry retries the
    // entire chain.
    {
        std::cout << "--- Combined: Retry + Fallback ---\n";

        std::vector<AnyChat> fallbacks;
        fallbacks.push_back(
            init_chat_model("openai:gpt-4o-mini", LLMConfig{.api_key = key}));

        auto agent = AgentExecutor{
            OpenAIChat{"nonexistent-model-xyz", key},
            AgentConfig{
                .name = "combined",
                .system_prompt = "Reply in one sentence.",
                .middlewares = {
                    middleware::model_retry({.max_retries = 1, .on_failure = "continue"}),
                    middleware::model_fallback(std::move(fallbacks), {}, log),
                },
            },
            log
        };

        auto result = agent.run("What year did humans land on the moon?");
        std::cout << "  " << result << "\n";
    }
}
