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

    std::cout << "=== Rationalize Middleware Demo ===\n"
              << "(Injects efficiency hints when large tool results are detected)\n\n";

    // Spy middleware: placed after rationalize in the chain so it sees
    // the temporarily-injected guidance in the system prompt.
    MiddlewareFn guidance_spy = [](std::vector<Message>& msgs, Next next) -> LLMResponse {
        if (!msgs.empty() && msgs.front().role == Role::system) {
            auto text = msgs.front().text();
            auto pos = text.find("[Efficiency guidance");
            if (pos != std::string::npos)
                std::cerr << "\n[spy] Rationalize middleware injected:\n"
                          << text.substr(pos) << "\n";
        }
        return next(msgs);
    };

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "devops",
            .system_prompt = "You are a devops assistant. Use tools to inspect the system. "
                             "Report findings concisely.",
            .tools = {
                Tool::create("get_logs", "Fetch recent system logs",
                    [](const json& p) -> json {
                        auto service = p["service"].get<std::string>();
                        // Simulate a large log dump (~1200 chars ≈ 300 tokens)
                        std::string logs = "=== Logs for " + service + " ===\n";
                        for (int i = 0; i < 30; ++i)
                            logs += "[2026-04-13T10:" + std::to_string(i) + ":00Z] "
                                    "INFO  " + service + " - Processing request #"
                                    + std::to_string(1000 + i)
                                    + " from client 192.168.1." + std::to_string(i % 256)
                                    + " status=200 duration=" + std::to_string(50 + i * 3)
                                    + "ms\n";
                        logs += "[2026-04-13T10:30:00Z] WARN  " + service
                              + " - High latency detected: 850ms (threshold: 500ms)\n"
                              + "[2026-04-13T10:31:00Z] ERROR " + service
                              + " - Connection pool exhausted, retrying...\n";
                        return logs;
                    },
                    {{"type", "object"},
                     {"properties", {{"service", {{"type", "string"},
                                                   {"description", "Service name"}}}}},
                     {"required", {"service"}}}),

                Tool::create("exec", "Run a shell command",
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
                     {"properties", {{"command", {{"type", "string"}}}}},
                     {"required", {"command"}}}),
            },
            .middlewares = {
                // Detects tool results > 50 tokens (~200 chars) and injects
                // efficiency hints into the system prompt for that LLM call only.
                middleware::rationalize({
                    .large_threshold = 50,
                    .hints = {
                        "Use grep/awk to filter logs instead of reading them entirely",
                        "Pipe command output through head/tail to limit result size",
                        "Use targeted searches rather than broad scans",
                    },
                }),
                guidance_spy,
            },
        },
        Log{std::cerr, LogLevel::info}
    };

    auto result = agent.run(
        "Fetch logs for the 'api-gateway' service. Then check disk usage. "
        "Report any issues found.");

    std::cout << "\nResult:\n" << result << "\n";
}
