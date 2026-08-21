// ═══════════════════════════════════════════════════════════════════════════════
//  17_streaming.cpp  —  Real SSE token streaming (OpenAI-compatible / Ollama)
//
//  Prints tokens to the terminal as they arrive off the wire, rather than
//  waiting for the whole response.  Uses the local Ollama endpoint (an
//  OpenAI-compatible server), so no API key is required — just a running Ollama:
//
//      ollama serve
//      ollama pull llama3
//
//  Override the model or endpoint with OLLAMA_MODEL / OLLAMA_BASE_URL.
// ═══════════════════════════════════════════════════════════════════════════════

#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/local.hpp>
#include <cstdlib>
#include <iostream>

int main() {
    using namespace tiny_agent;

    const char* model    = std::getenv("OLLAMA_MODEL");
    const char* base_url = std::getenv("OLLAMA_BASE_URL");

    LLMConfig cfg;
    if (base_url) cfg.base_url = base_url;
    auto llm = local::ollama(model ? model : "llama3", std::move(cfg));

    auto agent = make_agent(std::move(llm), {
        .system_prompt = "You are a concise assistant."
    });

    std::cout << "> Tell me a short story about a curious robot.\n\n";

    // run_stream drives the full ReAct loop; text deltas arrive live.  Flush
    // after each token so it renders immediately in the terminal.
    std::string final = agent.run_stream(
        "Tell me a short story about a curious robot.",
        [](const StreamEvent& e) {
            if (e.kind == StreamEvent::Kind::text_delta)
                std::cout << e.text << std::flush;
        });

    std::cout << "\n\n[complete] " << final.size() << " chars\n";
    return 0;
}
