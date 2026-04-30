#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::cout << "=== PII Protection Middleware Demo ===\n\n";

    // ── 1. REDACT — replace PII with [REDACTED_TYPE] ────────────────────

    {
        std::cout << "--- Strategy: REDACT ---\n";
        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "redact",
                .system_prompt = "Repeat the user's message back exactly as you receive it. "
                                 "Do not add or change anything.",
                .middlewares = {
                    middleware::pii({.pii_type = "email", .strategy = "redact"}),
                    middleware::pii({.pii_type = "ssn",   .strategy = "redact"}),
                    middleware::pii({.pii_type = "phone", .strategy = "redact"}),
                },
            }
        };

        std::string input = "Contact john.doe@example.com, SSN 123-45-6789, phone +1 555-123-4567";
        std::cout << "  Input:  " << input << "\n";
        std::cout << "  Output: " << agent.run(input) << "\n\n";
    }

    // ── 2. MASK — show only last 4 characters ───────────────────────────

    {
        std::cout << "--- Strategy: MASK ---\n";
        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "mask",
                .system_prompt = "Repeat the user's message back exactly as you receive it.",
                .middlewares = {
                    middleware::pii({.pii_type = "credit_card", .strategy = "mask"}),
                    middleware::pii({.pii_type = "email",       .strategy = "mask"}),
                },
            }
        };

        std::string input = "Card: 4111-1111-1111-1234, email: secret@corp.com";
        std::cout << "  Input:  " << input << "\n";
        std::cout << "  Output: " << agent.run(input) << "\n\n";
    }

    // ── 3. BLOCK — throw exception on PII detection ─────────────────────

    {
        std::cout << "--- Strategy: BLOCK ---\n";
        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "block",
                .system_prompt = "You are a helpful assistant.",
                .middlewares = {
                    middleware::pii({.pii_type = "ssn", .strategy = "block"}),
                },
            }
        };

        try {
            agent.run("My SSN is 999-88-7777, please store it.");
            std::cout << "  ERROR: Should have thrown!\n";
        } catch (const Error& e) {
            std::cout << "  Blocked! Exception: " << e.what() << "\n\n";
        }
    }

    // ── 4. OUTPUT scrubbing — redact PII in LLM responses ───────────────

    {
        std::cout << "--- Output Scrubbing ---\n";
        auto agent = AgentExecutor{
            OpenAIChat{"gpt-4o-mini", key},
            AgentConfig{
                .name = "output_scrub",
                .system_prompt = "Generate a fictional person profile: name, email "
                                 "(use @example.com), and US phone number.",
                .middlewares = {
                    middleware::pii({.pii_type = "email", .strategy = "redact",
                                    .apply_to_input = false, .apply_to_output = true}),
                    middleware::pii({.pii_type = "phone", .strategy = "redact",
                                    .apply_to_input = false, .apply_to_output = true}),
                },
            }
        };

        std::cout << "  (LLM generates a profile, PII redacted in output)\n";
        std::cout << "  Output: " << agent.run("Generate a profile.") << "\n";
    }
}
