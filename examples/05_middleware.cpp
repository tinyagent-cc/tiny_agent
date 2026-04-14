#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>
#include <chrono>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    MiddlewareFn timing = [](std::vector<Message>& msgs, Next next) -> LLMResponse {
        auto start = std::chrono::steady_clock::now();
        auto resp = next(msgs);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cerr << "[timing] LLM call took " << ms << "ms\n";
        return resp;
    };

    MiddlewareFn content_filter = [](std::vector<Message>& msgs, Next next) -> LLMResponse {
        for (auto& m : msgs)
            if (m.role == Role::user && m.text().find("ignore all") != std::string::npos)
                throw Error("prompt injection detected");
        return next(msgs);
    };

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .name = "guarded_agent",
            .system_prompt = "You are a helpful assistant.",
            .middlewares = {
                middleware::logging(Log{std::cerr, LogLevel::debug}),
                middleware::retry(2),
                middleware::trim_history(20),
                timing,
                content_filter,
            },
        },
        Log{std::cerr, LogLevel::debug}
    };

    std::cout << agent.run("Explain middleware in software in one sentence.") << "\n";
}
