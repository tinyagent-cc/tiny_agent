#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    auto llm = OpenAIChat{"gpt-4o-mini", key};

    auto response = llm.chat({
        Message::system("You are a concise assistant. Reply in one sentence."),
        Message::user("What is the capital of Japan?")
    });

    std::cout << response.message.text() << "\n";
    std::cout << "tokens: " << response.usage.dump() << "\n";
}
