#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/init_chat_model.hpp>
#include <tiny_agent/init_embeddings.hpp>
#include <iostream>
#include <cstdlib>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) { std::cerr << "OPENAI_API_KEY not set\n"; return 1; }

    // ── 1. Create an embeddings model ────────────────────────────────────

    auto embeddings = init_embeddings("openai:text-embedding-3-small",
        EmbeddingConfig{.api_key = key});

    // ── 2. Build a retriever and populate it ─────────────────────────────
    //
    // Retriever defaults to FlatVectorStore.  For hnswlib:
    //   #include <tiny_agent/vectorstore/hnswlib.hpp>
    //   auto r = Retriever{HnswVectorStore{1536, 10000}, std::move(embeddings)};

    auto retriever = Retriever{std::move(embeddings), /*top_k=*/3,
                               Log{std::cerr, LogLevel::debug}};

    retriever.add_documents({
        "The capital of France is Paris. It is known for the Eiffel Tower.",
        "Python is a programming language created by Guido van Rossum in 1991.",
        "The speed of light in a vacuum is approximately 299,792 km/s.",
        "Tokyo is the capital of Japan and the most populous metropolitan area.",
        "C++ was created by Bjarne Stroustrup at Bell Labs in 1979.",
        "The Great Wall of China stretches over 13,000 miles.",
        "Rust is a systems programming language focused on safety and performance.",
    });

    // ── 3. Create an agent that uses the retriever as a tool ─────────────

    auto agent = AgentExecutor{
        init_chat_model("openai:gpt-4o-mini", LLMConfig{.api_key = key}),
        AgentConfig{
            .name = "knowledge_agent",
            .system_prompt =
                "You answer questions using the search_knowledge tool to find "
                "relevant information. Always search before answering. Cite the "
                "information you found.",
            .tools = {
                retriever.as_tool("search_knowledge",
                    "Search the knowledge base for relevant information"),
            },
        },
        Log{std::cerr, LogLevel::debug}
    };

    auto answer = agent.run(
        "What programming languages are in the knowledge base and who created them?");
    std::cout << "\n" << answer << "\n";
}
