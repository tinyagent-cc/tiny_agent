// 19_vector_store — retrieval over a pluggable vector store
//
// The same Retriever code runs against an in-process store, a Qdrant server or
// a Chroma server. Which one is a config decision, not a code change: swap the
// store type, or pick it at runtime with AnyVectorStore as this example does.
//
//   ./build/examples/19_vector_store                          # in-memory
//   QDRANT_URL=http://localhost:6333 ./build/examples/19_vector_store
//   CHROMA_URL=http://localhost:8000 ./build/examples/19_vector_store
//
// Set OPENAI_API_KEY to use real embeddings; without it the example uses a
// deterministic local hash so it runs offline.

#include <tiny_agent/retriever.hpp>
#include <tiny_agent/vectorstore/chroma.hpp>
#include <tiny_agent/vectorstore/qdrant.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <cstdlib>
#include <iomanip>
#include <iostream>

using namespace tiny_agent;

// Stands in for a real embeddings model so the example runs with no API key.
struct HashEmbeddings {
    using input_t   = std::string;
    using output_t  = std::vector<float>;
    using model_tag = embedding_tag;

    static constexpr std::size_t kDims = 64;

    std::vector<float> embed_query(const std::string& text) const {
        std::vector<float> v(kDims, 0.0f);
        // Bag of character bigrams: crude, but similar strings land near each
        // other, which is all this example needs.
        for (std::size_t i = 0; i + 1 < text.size(); ++i) {
            auto h = (static_cast<unsigned>(static_cast<unsigned char>(text[i])) * 31u
                    + static_cast<unsigned>(static_cast<unsigned char>(text[i + 1]))) % kDims;
            v[h] += 1.0f;
        }
        return v;
    }

    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts) const {
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (const auto& t : texts) out.push_back(embed_query(t));
        return out;
    }

    std::vector<float> invoke(const std::string& t, const RunConfig& = {}) { return embed_query(t); }
    std::string model_name() const { return "hash-embed"; }
    std::size_t dimensions() const { return kDims; }
    std::vector<std::vector<float>> batch(std::vector<std::string> texts, const RunConfig& = {}) {
        return embed_documents(texts);
    }
    void stream(std::string t, std::function<void(std::vector<float>)> cb, const RunConfig& = {}) {
        cb(embed_query(t));
    }
};

static const std::vector<std::string> kCorpus = {
    "The Raspberry Pi 5 uses a Broadcom BCM2712 quad-core Arm Cortex-A76.",
    "llama.cpp serves an OpenAI-compatible endpoint on port 8080 by default.",
    "Cosine similarity measures the angle between two vectors, ignoring length.",
    "MCP lets an agent discover tools from a server over stdio or HTTP.",
    "Header-only C++ libraries need no build step from the consuming project.",
};

template<typename Store>
static void demo(Store store, const char* label) {
    std::cout << "\n=== " << label << " ===\n";

    Retriever<HashEmbeddings, Store> retriever{std::move(store), HashEmbeddings{}, 3};
    retriever.store().clear();

    std::vector<json> metadata;
    for (std::size_t i = 0; i < kCorpus.size(); ++i)
        metadata.push_back(json{{"source", "corpus"}, {"line", i}});
    retriever.add_documents(kCorpus, metadata);

    std::cout << "indexed " << retriever.store().size() << " documents\n";

    for (const auto* query : {"What CPU is in the Pi 5?", "how do agents find tools"}) {
        std::cout << "\nquery: " << query << "\n";
        for (const auto& hit : retriever.query(query, 2))
            std::cout << "  " << std::fixed << std::setprecision(3) << hit.score
                      << "  " << hit.content << "\n";
    }

    // The retriever also exposes itself as a tool an agent can call.
    auto tool = retriever.as_tool("search_docs", "Search the local document corpus");
    auto result = tool(json{{"query", "cosine"}, {"top_k", 1}});
    std::cout << "\nas a tool: " << result.dump() << "\n";

    retriever.store().clear();
}

int main() {
    const char* qdrant = std::getenv("QDRANT_URL");
    const char* chroma = std::getenv("CHROMA_URL");

    try {
        demo(FlatVectorStore{}, "FlatVectorStore (in-process)");

        if (qdrant) demo(QdrantVectorStore{qdrant, "tiny_agent_example"}, "Qdrant");
        else        std::cout << "\nset QDRANT_URL to run the Qdrant backend\n";

        if (chroma) demo(ChromaVectorStore{chroma, "tiny_agent_example"}, "Chroma");
        else        std::cout << "set CHROMA_URL to run the Chroma backend\n";

        // Picking a backend at runtime, when it comes from config rather than
        // from the type system.
        std::cout << "\n=== AnyVectorStore (backend chosen at runtime) ===\n";
        AnyVectorStore store = qdrant
            ? AnyVectorStore{QdrantVectorStore{qdrant, "tiny_agent_runtime"}}
            : AnyVectorStore{FlatVectorStore{}};
        Retriever<HashEmbeddings, AnyVectorStore> r{std::move(store), HashEmbeddings{}, 2};
        r.add_documents(kCorpus);
        std::cout << "indexed " << r.store().size() << " documents\n";
        for (const auto& hit : r.query("header only C++", 1))
            std::cout << "  " << std::fixed << std::setprecision(3) << hit.score
                      << "  " << hit.content << "\n";
        r.store().clear();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
