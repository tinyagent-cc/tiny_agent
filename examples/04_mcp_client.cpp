#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace tiny_agent;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <mcp-server-command> [args...]\n"
                  << "Example: " << argv[0] << " npx @modelcontextprotocol/server-filesystem /tmp\n";
        return 1;
    }

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    auto mcp = mcp::connect_stdio(command, args);
    auto tools = mcp.as_tools();

    std::cout << "Discovered " << tools.size() << " MCP tools:\n";
    for (auto& t : tools)
        std::cout << "  - " << t.schema.name << ": " << t.schema.description << "\n";

    auto agent = AgentExecutor{
        OpenAIChat{"gpt-4o-mini", key},
        AgentConfig{
            .name = "mcp_agent",
            .system_prompt = "You are a helpful assistant with access to external tools via MCP.",
            .tools = std::move(tools),
        },
        Log{std::cerr, LogLevel::debug}
    };

    auto result = agent.run("List the files in the current directory.");
    std::cout << "\n" << result << "\n";
}
