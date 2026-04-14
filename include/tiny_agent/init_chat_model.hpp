#pragma once
#include "core/llm.hpp"
#include "providers/openai.hpp"
#include "providers/anthropic.hpp"
#include "providers/gemini.hpp"
#include <algorithm>

namespace tiny_agent {

// ── Model-string parsing ────────────────────────────────────────────────────
//
// Accepts "provider:model" (explicit) or just "model" (auto-detected).
//   "openai:gpt-4o-mini"            → provider=openai,    model=gpt-4o-mini
//   "anthropic:claude-sonnet-4-20250514" → provider=anthropic, model=claude-sonnet-4-20250514
//   "gemini:gemini-2.0-flash"       → provider=gemini,    model=gemini-2.0-flash
//   "gpt-4o"                        → provider=openai     (auto-detected)
//   "claude-sonnet-4-20250514"           → provider=anthropic (auto-detected)

struct ModelSpec {
    std::string provider;
    std::string model;
};

inline ModelSpec parse_model_string(const std::string& model_string) {
    auto colon = model_string.find(':');
    if (colon != std::string::npos && colon > 0 && colon < model_string.size() - 1)
        return {model_string.substr(0, colon), model_string.substr(colon + 1)};

    auto starts = [&](std::string_view prefix) {
        return model_string.size() >= prefix.size() &&
               model_string.compare(0, prefix.size(), prefix) == 0;
    };

    if (starts("gpt-") || starts("o1-") || starts("o3-") || starts("chatgpt-"))
        return {"openai", model_string};
    if (starts("claude-"))
        return {"anthropic", model_string};
    if (starts("gemini-"))
        return {"gemini", model_string};

    return {"openai", model_string};   // default provider
}

// ── init_chat_model — provider-agnostic LLM factory ─────────────────────────
//
// Returns AnyLLM (type-erased) so the caller doesn't need to know the
// concrete provider at compile time.  Mirrors LangChain's init_chat_model().
//
//   auto llm = init_chat_model("openai:gpt-4o-mini",
//       LLMConfig{.api_key = getenv("OPENAI_API_KEY")});

inline AnyLLM init_chat_model(const std::string& model_string,
                               LLMConfig config = {}) {
    auto [provider, model] = parse_model_string(model_string);

    if (provider == "openai")
        return AnyLLM{LLM<openai>{model, std::move(config)}};
    if (provider == "anthropic")
        return AnyLLM{LLM<anthropic>{model, std::move(config)}};
    if (provider == "gemini")
        return AnyLLM{LLM<gemini>{model, std::move(config)}};

    throw Error("init_chat_model: unknown provider '" + provider +
                "' (supported: openai, anthropic, gemini)");
}

// Convenience: create from explicit provider + model strings.
inline AnyLLM init_chat_model(const std::string& provider,
                               const std::string& model,
                               LLMConfig config = {}) {
    return init_chat_model(provider + ":" + model, std::move(config));
}

} // namespace tiny_agent
