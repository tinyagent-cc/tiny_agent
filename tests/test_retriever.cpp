#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/retriever.hpp>
#include <tiny_agent/vectorstore/flat.hpp>
#include <tiny_agent/embeddings/core.hpp>

using namespace tiny_agent;

// ═══════════════════════════════════════════════════════════════════════════
// MockEmbeddings — deterministic embeddings for offline testing
// ═══════════════════════════════════════════════════════════════════════════

struct MockEmbeddings {
    std::vector<float> embed_query(const std::string& text) {
        return make_vec(text);
    }

    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts) {
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (auto& t : texts) out.push_back(make_vec(t));
        return out;
    }

    std::string_view model_name() const { return "mock-embed"; }

private:
    static std::vector<float> make_vec(const std::string& text) {
        // Deterministic: hash each char into a 3-dim vector
        float x = 0, y = 0, z = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            auto c = static_cast<float>(text[i]);
            x += c * (1.0f + static_cast<float>(i % 3 == 0));
            y += c * (1.0f + static_cast<float>(i % 3 == 1));
            z += c * (1.0f + static_cast<float>(i % 3 == 2));
        }
        float norm = std::sqrt(x*x + y*y + z*z);
        if (norm > 0) { x /= norm; y /= norm; z /= norm; }
        return {x, y, z};
    }
};

static_assert(embeddings_like<MockEmbeddings>);

// ═══════════════════════════════════════════════════════════════════════════
// Retriever — basic operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Retriever: add_documents and query") {
    auto retriever = Retriever{AnyEmbeddings{MockEmbeddings{}}, 2};

    retriever.add_documents({"alpha", "beta", "gamma"});

    auto results = retriever.query("alpha");
    REQUIRE(results.size() == 2);
    CHECK(results[0].score >= results[1].score);
}

TEST_CASE("Retriever: store size tracks documents") {
    auto retriever = Retriever{AnyEmbeddings{MockEmbeddings{}}};
    CHECK(retriever.store().size() == 0);

    retriever.add_documents({"one", "two", "three"});
    CHECK(retriever.store().size() == 3);
}

TEST_CASE("Retriever: query with custom top_k") {
    auto retriever = Retriever{AnyEmbeddings{MockEmbeddings{}}, 10};
    retriever.add_documents({"a", "b", "c"});

    auto results = retriever.query("a", 1);
    CHECK(results.size() == 1);
}

TEST_CASE("Retriever: metadata support") {
    auto retriever = Retriever{AnyEmbeddings{MockEmbeddings{}}};
    retriever.add_documents(
        {"doc with meta"},
        {json{{"source", "test"}}}
    );

    auto results = retriever.query("doc with meta", 1);
    REQUIRE(results.size() == 1);
    CHECK(results[0].metadata["source"] == "test");
}

// ═══════════════════════════════════════════════════════════════════════════
// Retriever — as_tool
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Retriever: as_tool returns valid tool") {
    auto retriever = Retriever{AnyEmbeddings{MockEmbeddings{}}};
    retriever.add_documents({"hello world"});

    auto tool = retriever.as_tool("search", "Search docs");

    CHECK(tool.schema.name == "search");
    CHECK(tool.schema.description == "Search docs");
    CHECK(tool.schema.parameters["required"][0] == "query");
}

TEST_CASE("Retriever: tool invocation returns JSON results") {
    auto retriever = Retriever{AnyEmbeddings{MockEmbeddings{}}, 2};
    retriever.add_documents({"first doc", "second doc", "third doc"});

    auto tool = retriever.as_tool("search", "Search");
    auto result = tool({{"query", "first doc"}});

    CHECK(result.is_array());
    REQUIRE(result.size() == 2);
    CHECK(result[0].contains("content"));
    CHECK(result[0].contains("score"));
}

// ═══════════════════════════════════════════════════════════════════════════
// Retriever — templated on store type (compile-time check)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Retriever<FlatVectorStore> explicit template") {
    Retriever<FlatVectorStore> retriever{AnyEmbeddings{MockEmbeddings{}}};
    retriever.add_documents({"test"});
    CHECK(retriever.store().size() == 1);
}

TEST_CASE("Retriever with pre-built store") {
    FlatVectorStore store;
    store.add("pre", "pre-existing", {1, 0, 0}, json::object());

    auto retriever = Retriever{std::move(store), AnyEmbeddings{MockEmbeddings{}}};
    CHECK(retriever.store().size() == 1);
}
