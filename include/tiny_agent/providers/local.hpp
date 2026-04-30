#pragma once
#include "openai.hpp"

namespace tiny_agent::local {

inline auto ollama(std::string model = "llama3", LLMConfig cfg = {}) {
    if (cfg.base_url.empty()) cfg.base_url = "http://localhost:11434";
    return LLMModel<OpenAI, chat_tag>{std::move(model), std::move(cfg)};
}

inline auto llamacpp(std::string model = "default", LLMConfig cfg = {}) {
    if (cfg.base_url.empty()) cfg.base_url = "http://localhost:8080";
    return LLMModel<OpenAI, chat_tag>{std::move(model), std::move(cfg)};
}

inline auto vllm(std::string model, LLMConfig cfg = {}) {
    if (cfg.base_url.empty()) cfg.base_url = "http://localhost:8000";
    return LLMModel<OpenAI, chat_tag>{std::move(model), std::move(cfg)};
}

inline auto create(std::string model, std::string base_url, LLMConfig cfg = {}) {
    cfg.base_url = std::move(base_url);
    return LLMModel<OpenAI, chat_tag>{std::move(model), std::move(cfg)};
}

} // namespace tiny_agent::local
