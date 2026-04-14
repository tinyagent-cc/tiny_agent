#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <string>

static std::string shell_exec(const std::string& cmd) {
    std::array<char, 256> buf;
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "error: popen failed";
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        output += buf.data();
    pclose(pipe);
    return output;
}

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    std::cout << "=== Full Middleware Stack Demo ===\n"
              << "All 12 built-in middleware working together:\n"
              << "  logging, system_prompt, pii, trim_history, summarize,\n"
              << "  model_call_limit, tool_call_limit, model_retry,\n"
              << "  context_editing, rationalize, retry\n\n";

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "full_stack",
            // No system_prompt here — middleware injects one.
            .tools = {
                Tool::create("exec", "Run a shell command",
                    [](const json& p) -> json {
                        return shell_exec(p["command"].get<std::string>());
                    },
                    {{"type", "object"},
                     {"properties", {{"command", {{"type", "string"},
                                                   {"description", "Shell command"}}}}},
                     {"required", {"command"}}}),

                Tool::create("word_count", "Count words in text",
                    [](const json& p) -> json {
                        auto text = p["text"].get<std::string>();
                        int words = 0;
                        bool in_word = false;
                        for (char c : text) {
                            if (std::isspace(static_cast<unsigned char>(c))) {
                                in_word = false;
                            } else if (!in_word) {
                                in_word = true;
                                ++words;
                            }
                        }
                        return json{{"words", words}, {"chars", text.size()}};
                    },
                    {{"type", "object"},
                     {"properties", {{"text", {{"type", "string"}}}}},
                     {"required", {"text"}}}),

                Tool::create("lookup", "Look up information on a topic",
                    [](const json& p) -> json {
                        auto topic = p["topic"].get<std::string>();
                        // Simulated knowledge base with substantial content
                        std::string result = "Knowledge base entry for '" + topic + "':\n";
                        result += "This topic encompasses several key areas including "
                                  "theoretical foundations, practical applications, and "
                                  "recent developments. Research indicates significant "
                                  "progress in the last decade with measurable improvements "
                                  "across multiple metrics. Current best practices suggest "
                                  "an integrated approach combining traditional methods "
                                  "with modern techniques for optimal results.";
                        return result;
                    },
                    {{"type", "object"},
                     {"properties", {{"topic", {{"type", "string"}}}}},
                     {"required", {"topic"}}}),
            },
            .middlewares = {
                // 1. Logging — outermost, captures everything
                middleware::logging(Log{std::cerr, LogLevel::debug}),

                // 2. PII protection — scrub emails before they reach the LLM
                middleware::pii({.pii_type = "email", .strategy = "redact"}),

                // 3. System prompt — inject persona since config has none
                middleware::system_prompt(
                    "You are a helpful research assistant with access to shell "
                    "commands and a knowledge base. Be thorough but concise."),

                // 4. Retry on transient server errors
                middleware::retry(2, std::chrono::milliseconds(500)),

                // 5. Enhanced retry with backoff
                middleware::model_retry({.max_retries = 1, .initial_delay = 300.0}),

                // 6. Cap total LLM calls to prevent runaway loops
                middleware::model_call_limit({.limit = 8}),

                // 7. Cap total tool calls
                middleware::tool_call_limit({.limit = 6}),

                // 8. Keep history bounded in chat mode
                middleware::trim_history(20),

                // 9. Compress old conversation into summaries
                middleware::summarize(middleware::SummarizeConfig{
                    .trigger_tokens = 500,
                    .keep_recent    = 4,
                }),

                // 10. Clear old tool results when context is large
                middleware::context_editing({.trigger = 400, .keep = 2}),

                // 11. Inject efficiency hints for large tool results
                middleware::rationalize({.large_threshold = 100}),
            },
            .max_iterations = 10,
        },
        Log{std::cerr, LogLevel::debug}
    };

    // Single-shot with tools — triggers logging, system_prompt, pii,
    // model_call_limit, tool_call_limit, and potentially rationalize.
    std::cout << "--- Single-shot query with tools ---\n";
    auto result = agent.run(
        "My email is analyst@company.com. "
        "Run 'uname -s' and 'whoami', then look up 'machine learning'. "
        "Summarize what you found.");
    std::cout << "\nResult:\n" << result << "\n\n";

    // Multi-turn chat — additionally triggers trim_history, summarize,
    // and context_editing as the conversation grows.
    std::cout << "--- Multi-turn chat ---\n";
    const char* turns[] = {
        "Look up 'renewable energy' and count the words in the result.",
        "Now look up 'climate change' and compare with the previous topic.",
        "Run 'date' and 'uptime'. What time is it?",
        "Summarize everything we've discussed so far.",
    };

    for (auto* turn : turns) {
        std::cout << "\nUser: " << turn << "\n";
        auto reply = agent.chat(turn);
        std::cout << "Agent: " << reply.substr(0, 200)
                  << (reply.size() > 200 ? "..." : "") << "\n"
                  << "[history=" << agent.history().size() << " messages]\n";
    }
}
