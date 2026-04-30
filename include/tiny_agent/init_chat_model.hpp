#pragma once
#include "core/model.hpp"
#include "providers/openai.hpp"
#include "providers/anthropic.hpp"
#include "providers/gemini.hpp"
#include <algorithm>

namespace tiny_agent {

using AnyChat = ChatVariant<OpenAI, Anthropic, Gemini>;
static_assert(is_chat<AnyChat>);

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

    return {"openai", model_string};
}

inline AnyChat init_chat_model(const std::string& model_string,
                               LLMConfig config = {}) {
    auto [provider, model] = parse_model_string(model_string);

    if (provider == "openai")
        return AnyChat{LLMModel<OpenAI, chat_tag>{model, std::move(config)}};
    if (provider == "anthropic")
        return AnyChat{LLMModel<Anthropic, chat_tag>{model, std::move(config)}};
    if (provider == "gemini")
        return AnyChat{LLMModel<Gemini, chat_tag>{model, std::move(config)}};

    throw Error("init_chat_model: unknown provider '" + provider +
                "' (supported: openai, anthropic, gemini)");
}

inline AnyChat init_chat_model(const std::string& provider,
                               const std::string& model,
                               LLMConfig config = {}) {
    return init_chat_model(provider + ":" + model, std::move(config));
}

} // namespace tiny_agent
