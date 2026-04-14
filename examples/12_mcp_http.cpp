#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    using namespace tiny_agent;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <mcp-server-url> [endpoint]\n"
                  << "Example: " << argv[0] << " http://localhost:3000 /mcp\n";
        return 1;
    }

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::string base_url = argv[1];
    std::string endpoint = argc > 2 ? argv[2] : "/mcp";

    auto mcp = mcp::connect_http(base_url, endpoint,
        Log{std::cerr, LogLevel::debug});
    auto tools = mcp.as_tools();

    std::cout << "Discovered " << tools.size() << " MCP tools:\n";
    for (auto& t : tools)
        std::cout << "  - " << t.schema.name << ": " << t.schema.description << "\n";

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "mcp_http_agent",
            .system_prompt = "You are a helpful assistant with access to external tools via MCP.",
            .tools = std::move(tools),
        },
        Log{std::cerr, LogLevel::debug}
    };

    auto result = agent.run("What tools do you have available? Briefly describe each.");
    std::cout << "\n" << result << "\n";
}
