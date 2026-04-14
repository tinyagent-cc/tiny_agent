#pragma once
#include "../core/middleware.hpp"
#include "../core/llm.hpp"
#include <memory>

namespace tiny_agent::middleware {

// Automatically fall back to alternative models when the primary model fails.
// Inspired by LangChain's ModelFallbackMiddleware.
//
// Because AnyLLM is move-only, fallback state lives in a shared_ptr so the
// resulting MiddlewareFn (std::function) stays copyable.

inline MiddlewareFn model_fallback(
    std::vector<AnyLLM> fallback_llms,
    std::vector<ToolSchema> schemas = {},
    Log log = {})
{
    struct State {
        std::vector<AnyLLM> llms;
        std::vector<ToolSchema> schemas;
        Log log;
    };
    auto state = std::make_shared<State>();
    for (auto& llm : fallback_llms) state->llms.push_back(std::move(llm));
    state->schemas = std::move(schemas);
    state->log     = std::move(log);

    return [state](std::vector<Message>& msgs, Next next) -> LLMResponse {
        try {
            return next(msgs);
        } catch (const APIError& primary_err) {
            state->log.warn("model_fallback",
                "primary model failed: " + std::string(primary_err.what()));

            for (std::size_t i = 0; i < state->llms.size(); ++i) {
                try {
                    state->log.info("model_fallback",
                        "trying fallback " + std::to_string(i + 1) +
                        "/" + std::to_string(state->llms.size()) +
                        " (" + std::string(state->llms[i].model_name()) + ")");
                    return state->llms[i].chat(msgs, state->schemas);
                } catch (const APIError& fallback_err) {
                    state->log.warn("model_fallback",
                        "fallback " + std::to_string(i + 1) + " failed: " +
                        std::string(fallback_err.what()));
                }
            }
            throw;   // all fallbacks exhausted → rethrow primary
        }
    };
}

} // namespace tiny_agent::middleware
