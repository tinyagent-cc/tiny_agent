#pragma once
#include "../core/middleware.hpp"
#include "../init_chat_model.hpp"
#include <memory>

namespace tiny_agent::middleware {

inline MiddlewareFn model_fallback(
    std::vector<AnyChat> fallback_llms,
    std::vector<ToolSchema> schemas = {},
    Log log = {})
{
    struct State {
        std::vector<AnyChat> llms;
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
            throw;
        }
    };
}

} // namespace tiny_agent::middleware
