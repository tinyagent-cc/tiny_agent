#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <tiny_agent/providers/anthropic.hpp>
#include <tiny_agent/providers/gemini.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    // ── Override kwargs per-call via LLMConfig ──────────────────────────────
    // Each provider's chat() accepts an optional LLMConfig overrides param.
    // Fields you set are merged onto the model's base config for that call only.

    auto llm = OpenAIChat{.model="gpt-4o-mini", .api_key=key};

    // Call with lower temperature for more deterministic output
    auto response = llm.chat(
        {Message::user("Say 'hello' in exactly 3 words.")},
        {},
        LLMConfig{.temperature = 0.0}
    );
    std::cout << "Low-temp response: " << response.message.text() << "\n";

    // ── bind() — create a pre-configured model wrapper ─────────────────────
    // Similar to LangChain's model.bind(temperature=0.8, ...)

    auto creative_model = bind(
        OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
        LLMConfig{.temperature = 0.9, .max_tokens = 50}
    );

    auto creative_resp = creative_model.chat(
        {Message::user("Invent a one-sentence fantasy story.")}
    );
    std::cout << "Creative model: " << creative_resp.message.text() << "\n";

    // ── Agent with per-invoke kwargs ───────────────────────────────────────
    // Pass LLMConfig overrides to agent.invoke()

    auto agent = make_agent(
        OpenAIChat{.model="gpt-4o-mini", .api_key=key},
        {
            .name = "kwargs_demo",
            .system_prompt = "Answer very concisely.",
            .tools = {}
        }
    );

    // This invoke uses temperature=0.0 for deterministic output
    auto answer = agent.invoke(
        "What is 2+2? Reply with just the number.",
        LLMConfig{.temperature = 0.0}
    );
    std::cout << "Agent (temp=0): " << answer << "\n";

    // ── Runnable piping ────────────────────────────────────────────────────
    // Compose runnables with the | operator (LangChain-style)

    auto upper = make_runnable<std::string, std::string>(
        [](std::string s) { return s + " (uppercased)"; }
    );

    auto pipeline = std::move(agent) | std::move(upper);
    auto piped_result = pipeline.invoke("What is the capital of France?");
    std::cout << "Piped result: " << piped_result << "\n";

    return 0;
}
