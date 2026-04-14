#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;
    using namespace tiny_agent::skills;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    // Discover skills from the examples/skills/ directory.
    SkillRegistry registry;
    registry.add_from_directory("examples/skills");

    std::cout << "Loaded " << registry.size() << " skills:\n";
    for (auto& name : registry.list())
        std::cout << "  - " << name << ": "
                  << registry.get(name).description.substr(0, 60) << "...\n";

    // Build a system prompt that includes selected skills.
    auto skill_prompt = registry.build_prompt({"code-review", "summarize"});

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "skilled_agent",
            .system_prompt = "You are an expert assistant.\n\n" + skill_prompt,
        }
    };

    std::cout << "\n--- Agent with code-review + summarize skills ---\n";
    std::cout << agent.run(
        "Review this code snippet:\n"
        "```cpp\n"
        "int* get_data() {\n"
        "    int x = 42;\n"
        "    return &x;\n"
        "}\n"
        "```") << "\n";
}
