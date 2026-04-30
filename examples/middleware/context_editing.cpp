#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <string>

static std::string make_report(const std::string& topic) {
    // ~800 chars per report to push tokens past the trigger threshold
    return "=== Report: " + topic + " ===\n"
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod "
        "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, "
        "quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo "
        "consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse "
        "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat "
        "non proident, sunt in culpa qui officia deserunt mollit anim id est laborum. "
        "Key findings for " + topic + ": significant improvements observed across "
        "all metrics with a 23% increase in efficiency and 15% reduction in costs. "
        "Recommendations: continue current approach with minor adjustments to the "
        "secondary parameters. End of report for " + topic + ".";
}

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::cout << "=== Context Editing Middleware Demo ===\n"
              << "(Old tool results replaced with [cleared] when token budget exceeded)\n\n";

    auto agent = AgentExecutor{
        OpenAIChat{"gpt-4o-mini", key},
        AgentConfig{
            .name = "analyst",
            .system_prompt = "You are a research analyst. Use read_report to gather data. "
                             "After reading all requested reports, provide a brief combined summary.",
            .tools = {
                DynamicTool::create("read_report", "Read a research report by topic",
                    [](const json& p) -> json {
                        return make_report(p["topic"].get<std::string>());
                    },
                    {{"type", "object"},
                     {"properties", {{"topic", {{"type", "string"},
                                                 {"description", "Report topic"}}}}},
                     {"required", {"topic"}}}),
            },
            .middlewares = {
                // Low trigger so it fires after 2-3 tool results (~200 tokens each).
                // keep=1 means only the most recent tool result survives.
                middleware::context_editing({
                    .trigger  = 300,
                    .keep     = 1,
                    .placeholder = "[cleared — old result removed to save context]",
                }),
            },
        },
        Log{std::cerr, LogLevel::debug}
    };

    auto result = agent.run(
        "Read reports on: 'solar energy', 'wind power', 'battery storage', and "
        "'grid modernization'. Then give a one-paragraph summary of all findings.");

    std::cout << "\nResult:\n" << result << "\n";

    // Show the same behavior in chat mode so we can inspect history
    std::cout << "\n--- Chat mode (inspectable history) ---\n";

    auto chat_agent = AgentExecutor{
        OpenAIChat{"gpt-4o-mini", key},
        AgentConfig{
            .name = "chat_analyst",
            .system_prompt = "You are a research assistant. Read reports when asked. "
                             "Respond briefly.",
            .tools = {
                DynamicTool::create("read_report", "Read a research report by topic",
                    [](const json& p) -> json {
                        return make_report(p["topic"].get<std::string>());
                    },
                    {{"type", "object"},
                     {"properties", {{"topic", {{"type", "string"}}}}},
                     {"required", {"topic"}}}),
            },
            .middlewares = {
                middleware::context_editing({
                    .trigger = 300, .keep = 1,
                    .placeholder = "[cleared]",
                }),
            },
        },
    };

    chat_agent.chat("Read the report on 'solar energy'.");
    chat_agent.chat("Now read the report on 'wind power'.");
    chat_agent.chat("And read the report on 'battery storage'.");

    std::cout << "\nFinal history (" << chat_agent.history().size() << " messages):\n";
    for (std::size_t i = 0; i < chat_agent.history().size(); ++i) {
        auto& m = chat_agent.history()[i];
        auto text = m.text();
        bool cleared = text.find("[cleared]") != std::string::npos;
        std::cout << "  [" << i << "] " << to_string(m.role)
                  << (cleared ? " [CLEARED]" : "")
                  << ": " << text.substr(0, 80)
                  << (text.size() > 80 ? "..." : "") << "\n";
    }
}
