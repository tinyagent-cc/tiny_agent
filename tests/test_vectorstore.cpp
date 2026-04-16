#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/vectorstore/base.hpp>
#include <tiny_agent/vectorstore/flat.hpp>

using namespace tiny_agent;

// ═══════════════════════════════════════════════════════════════════════════
// Concept satisfaction
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("FlatVectorStore satisfies vector_store concept") {
    static_assert(vector_store<FlatVectorStore>);
}

// ═══════════════════════════════════════════════════════════════════════════
// FlatVectorStore — basic operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("FlatVectorStore: add and size") {
    FlatVectorStore store;
    CHECK(store.size() == 0);
    store.add("id1", "hello", {1.0f, 0.0f, 0.0f}, json::object());
    CHECK(store.size() == 1);
    store.add("id2", "world", {0.0f, 1.0f, 0.0f}, json::object());
    CHECK(store.size() == 2);
}

TEST_CASE("FlatVectorStore: clear") {
    FlatVectorStore store;
    store.add("id1", "a", {1, 0, 0}, json::object());
    store.add("id2", "b", {0, 1, 0}, json::object());
    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("FlatVectorStore: auto-id add") {
    FlatVectorStore store;
    store.add("content", {1, 0, 0});
    CHECK(store.size() == 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// FlatVectorStore — cosine similarity search
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("FlatVectorStore: search returns exact match first") {
    FlatVectorStore store;
    store.add("x", "along x", {1, 0, 0}, json::object());
    store.add("y", "along y", {0, 1, 0}, json::object());
    store.add("z", "along z", {0, 0, 1}, json::object());

    auto results = store.search({1, 0, 0}, 3);
    REQUIRE(results.size() == 3);
    CHECK(results[0].id == "x");
    CHECK(results[0].score == doctest::Approx(1.0f));
    CHECK(results[1].score == doctest::Approx(0.0f));
    CHECK(results[2].score == doctest::Approx(0.0f));
}

TEST_CASE("FlatVectorStore: search ranks by similarity") {
    FlatVectorStore store;
    store.add("close",  "close",  {0.9f, 0.1f, 0.0f}, json::object());
    store.add("medium", "medium", {0.5f, 0.5f, 0.0f}, json::object());
    store.add("far",    "far",    {0.0f, 1.0f, 0.0f}, json::object());

    auto results = store.search({1.0f, 0.0f, 0.0f}, 3);
    REQUIRE(results.size() == 3);
    CHECK(results[0].id == "close");
    CHECK(results[1].id == "medium");
    CHECK(results[2].id == "far");
    CHECK(results[0].score > results[1].score);
    CHECK(results[1].score > results[2].score);
}

TEST_CASE("FlatVectorStore: top_k limits results") {
    FlatVectorStore store;
    store.add("a", "a", {1, 0, 0}, json::object());
    store.add("b", "b", {0, 1, 0}, json::object());
    store.add("c", "c", {0, 0, 1}, json::object());

    auto results = store.search({1, 0, 0}, 1);
    CHECK(results.size() == 1);
    CHECK(results[0].id == "a");
}

TEST_CASE("FlatVectorStore: top_k larger than store size") {
    FlatVectorStore store;
    store.add("only", "only doc", {1, 0, 0}, json::object());

    auto results = store.search({1, 0, 0}, 10);
    CHECK(results.size() == 1);
}

TEST_CASE("FlatVectorStore: empty store search") {
    FlatVectorStore store;
    auto results = store.search({1, 0, 0}, 5);
    CHECK(results.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// FlatVectorStore — metadata
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("FlatVectorStore: metadata preserved in search results") {
    FlatVectorStore store;
    json meta = {{"source", "test"}, {"page", 42}};
    store.add("doc1", "content", {1, 0, 0}, meta);

    auto results = store.search({1, 0, 0}, 1);
    REQUIRE(results.size() == 1);
    CHECK(results[0].metadata["source"] == "test");
    CHECK(results[0].metadata["page"] == 42);
}

TEST_CASE("FlatVectorStore: content preserved in search results") {
    FlatVectorStore store;
    store.add("id1", "the actual content", {1, 0, 0}, json::object());

    auto results = store.search({1, 0, 0}, 1);
    REQUIRE(results.size() == 1);
    CHECK(results[0].content == "the actual content");
}

// ═══════════════════════════════════════════════════════════════════════════
// FlatVectorStore — error cases
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("FlatVectorStore: dimension mismatch throws") {
    FlatVectorStore store;
    store.add("id1", "doc", {1, 0, 0}, json::object());

    CHECK_THROWS_AS(store.search({1, 0}, 1), Error);
}
