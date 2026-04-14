#pragma once
#include "openai.hpp"

namespace tiny_agent::local {

// Local providers reuse the OpenAI-compatible protocol with custom base URLs.
// Returns LLM<openai> since Ollama/llama.cpp/vLLM all speak OpenAI's wire format.

inline auto ollama(std::string model = "llama3", LLMConfig cfg = {}) {
    if (cfg.base_url.empty()) cfg.base_url = "http://localhost:11434";
    return LLM<openai>{std::move(model), std::move(cfg)};
}

inline auto llamacpp(std::string model = "default", LLMConfig cfg = {}) {
    if (cfg.base_url.empty()) cfg.base_url = "http://localhost:8080";
    return LLM<openai>{std::move(model), std::move(cfg)};
}

inline auto vllm(std::string model, LLMConfig cfg = {}) {
    if (cfg.base_url.empty()) cfg.base_url = "http://localhost:8000";
    return LLM<openai>{std::move(model), std::move(cfg)};
}

inline auto create(std::string model, std::string base_url, LLMConfig cfg = {}) {
    cfg.base_url = std::move(base_url);
    return LLM<openai>{std::move(model), std::move(cfg)};
}

} // namespace tiny_agent::local
