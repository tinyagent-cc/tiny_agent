#pragma once
#include "../core/types.hpp"
#include "../core/tool.hpp"
#include "../core/middleware.hpp"
#include "../core/log.hpp"
#include "../core/model.hpp"
#include <string>
#include <vector>
#include <memory>

namespace tiny_agent::agents {

struct deep_agent_tag {};

struct AgentConfig {
    std::string name = "agent";
    std::string system_prompt;
    std::vector<DynamicTool>   tools;
    std::vector<MiddlewareFn>  middlewares;
    int max_iterations = 10;
    LLMConfig llm_config;
    Log logger;
    json kwargs = json::object();
};

} // namespace tiny_agent::agents
