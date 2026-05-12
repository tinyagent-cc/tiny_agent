#pragma once
#include "agents/base.hpp"
#include "agents/deep_agent.hpp"
#include "agents/create.hpp"

namespace tiny_agent {
    using agents::AgentConfig;
    using agents::DeepAgent;
    using agents::deep_agent_tag;
    template<typename T>
    using AgentExecutor = agents::DeepAgent<T>;
}
