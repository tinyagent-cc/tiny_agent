#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <array>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "shell_agent",
            .system_prompt = "You are a system admin assistant. Use the exec tool to run commands. "
                             "Report the output concisely.",
            .tools = {
                Tool::create("exec", "Run a shell command and return stdout",
                    [](const json& p) -> json {
                        auto cmd = p["command"].get<std::string>();
                        std::array<char, 256> buf;
                        std::string output;
                        FILE* pipe = popen(cmd.c_str(), "r");
                        if (!pipe) return "error: popen failed";
                        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
                            output += buf.data();
                        pclose(pipe);
                        return output;
                    },
                    {{"type", "object"},
                     {"properties", {{"command", {{"type", "string"}, {"description", "Shell command to run"}}}}},
                     {"required", {"command"}}}),
            },
            .middlewares = {
                // Logs message count, role summaries, response timing, and tool call count
                // for every LLM call. Output goes to stderr at debug/trace level.
                middleware::logging(Log{std::cerr, LogLevel::trace}),
            },
        },
        Log{std::cerr, LogLevel::debug}
    };

    std::cout << "=== Logging Middleware Demo ===\n"
              << "(Watch stderr for per-call middleware logs)\n\n";

    auto result = agent.run(
        "Run 'echo Hello from tiny_agent' and 'date +%Y-%m-%d'. Report both outputs.");
    std::cout << "\nResult:\n" << result << "\n";
}
