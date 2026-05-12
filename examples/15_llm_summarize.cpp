#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <sstream>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    auto llm = OpenAIChat{.model="gpt-4o-mini", .api_key=key};

    // ── LLM-based summarization ─────────────────────────────────────────────
    // Instead of extractive truncation, the summarize middleware can call an LLM
    // to produce a real summary.  When total tokens exceed trigger_tokens, older
    // messages are chunked, each chunk is summarized via llm_fn, and chunk
    // summaries are further condensed if needed.

    // Build an llm_fn: a callable that takes a prompt string and returns text.
    auto llm_summarizer = [&llm](const std::string& prompt) -> std::string {
        auto resp = llm.chat({Message::user(prompt)});
        return resp.message.text();
    };

    // Create the middleware with LLM summarization enabled
    auto summarize_mw = middleware::summarize({
        .trigger_tokens = 100,     // trigger at ~100 chars to demonstrate
        .keep_recent = 2,          // keep the 2 most recent messages
        .chunk_tokens = 500,       // chunk size for LLM summarization
        .prompt = middleware::DEFAULT_SUMMARIZE_PROMPT,
        .llm_fn = llm_summarizer   // when set, uses LLM instead of extractive
    });

    // ── Build a conversation that triggers summarization ──────────────────

    std::vector<Message> conversation = {
        Message::system("You are a helpful assistant."),
        Message::user("What is the theory of relativity?"),
        Message::assistant("Einstein's theory of relativity consists of "
            "special relativity (1905) and general relativity (1915). "
            "Special relativity says the laws of physics are the same in all "
            "inertial frames and the speed of light is constant. General relativity "
            "describes gravity as the curvature of spacetime caused by mass."),
        Message::user("Tell me about quantum mechanics."),
        Message::assistant("Quantum mechanics describes nature at the smallest "
            "scales. Key principles include superposition (particles exist in "
            "multiple states until measured), wave-particle duality, and "
            "quantum entanglement. The Schrodinger equation governs how quantum "
            "systems evolve over time."),
        Message::user("How do they relate to each other?"),
    };

    std::cout << "Before summarization: " << conversation.size() << " messages\n";

    // Apply the summarize middleware
    auto response = summarize_mw(conversation, [](auto& msgs) -> LLMResponse {
        std::cout << "After summarization: " << msgs.size() << " messages\n";
        for (auto& m : msgs) {
            std::cout << "  [" << to_string(m.role) << "] "
                      << m.text().substr(0, 80) << "...\n";
        }
        return {Message::assistant("done"), {}, "stop", {}};
    });

    std::cout << "\nFinal response: " << response.message.text() << "\n";

    // ── Or use the default extractive fallback (no LLM call) ──────────────
    auto extractive_mw = middleware::summarize({
        .trigger_tokens = 100,
        .keep_recent = 2,
        .fallback = nullptr  // uses built-in extractive summarizer
    });

    std::vector<Message> convo2 = {
        Message::system("Assistant."),
        Message::user("Message 1: some content here"),
        Message::assistant("Response 1"),
        Message::user("Message 2: more content"),
        Message::assistant("Response 2"),
    };

    std::cout << "\nExtractive fallback:\n";
    extractive_mw(convo2, [](auto& msgs) -> LLMResponse {
        for (auto& m : msgs)
            std::cout << "  [" << to_string(m.role) << "] " << m.text() << "\n";
        return {Message::assistant("done"), {}, "stop", {}};
    });

    return 0;
}
