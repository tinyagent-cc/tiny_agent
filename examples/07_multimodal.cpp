#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    auto llm = LLM<openai>{"gpt-4o-mini", key};

    auto response = llm.chat({
        Message::system("Describe images concisely."),
        Message::image(
            "What is in this image?",
            "https://upload.wikimedia.org/wikipedia/commons/thumb/3/3a/Cat03.jpg/1200px-Cat03.jpg"
        )
    });

    std::cout << response.message.text() << "\n";
}
