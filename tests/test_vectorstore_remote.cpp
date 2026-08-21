#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/vectorstore/base.hpp>
#include <tiny_agent/vectorstore/flat.hpp>
#include <tiny_agent/vectorstore/qdrant.hpp>
#include <tiny_agent/vectorstore/chroma.hpp>
#include <tiny_agent/retriever.hpp>
#include "mock_model.hpp"
#include <cstdlib>

using namespace tiny_agent;
using tiny_agent::test::MockEmbed;

// ═══════════════════════════════════════════════════════════════════════════
// AnyVectorStore — the runtime-chosen backend
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("AnyVectorStore forwards the whole interface") {
    AnyVectorStore store = FlatVectorStore{};
    CHECK(store.size() == 0);

    store.add("a", "alpha", {1.0f, 0.0f, 0.0f}, json::object());
    store.add("b", "beta",  {0.0f, 1.0f, 0.0f}, json{{"tag", "x"}});
    CHECK(store.size() == 2);

    auto hits = store.search({1.0f, 0.0f, 0.0f}, 2);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "a");
    CHECK(hits[0].content == "alpha");
    CHECK(hits[0].score > hits[1].score);

    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("AnyVectorStore batches through a store without add_batch") {
    AnyVectorStore store = FlatVectorStore{};
    store.add_batch({{"a", "alpha", {1, 0, 0}, json::object()},
                     {"b", "beta",  {0, 1, 0}, json::object()}});
    CHECK(store.size() == 2);
}

TEST_CASE("AnyVectorStore satisfies the concept it erases") {
    static_assert(vector_store<AnyVectorStore>);
    static_assert(batch_vector_store<ChromaVectorStore>);
    static_assert(batch_vector_store<QdrantVectorStore>);
}

TEST_CASE("a Retriever works over a runtime-selected store") {
    Retriever<MockEmbed, AnyVectorStore> r{AnyVectorStore{FlatVectorStore{}}, MockEmbed{}, 2};
    r.add_documents({"the cat sat", "the dog ran", "quantum mechanics"});
    CHECK(r.store().size() == 3);
    auto hits = r.query("the cat sat", 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "the cat sat");
}

// ═══════════════════════════════════════════════════════════════════════════
// Qdrant — payload shaping, offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Qdrant point ids are UUIDs, deterministically derived") {
    auto a = detail::uuid_from_string("doc_0");
    auto b = detail::uuid_from_string("doc_0");
    auto c = detail::uuid_from_string("doc_1");

    CHECK(a == b);                     // same input, same point
    CHECK(a != c);
    CHECK(a.size() == 36);
    CHECK(a[8] == '-');
    CHECK(a[13] == '-');
    CHECK(a[14] == '4');               // version nibble
    CHECK(a[18] == '-');
    CHECK(a[19] == 'a');               // variant nibble
    CHECK(a[23] == '-');
    CHECK(a.find_first_not_of("0123456789abcdef-") == std::string::npos);
    CHECK(a != "00000000-0000-4000-a000-000000000000");
}

TEST_CASE("Qdrant upsert body carries the original id in the payload") {
    auto body = QdrantVectorStore::build_upsert_body(
        {{"doc_0", "hello", {1.0f, 2.0f}, json{{"src", "test"}}}});

    REQUIRE(body["points"].size() == 1);
    auto& p = body["points"][0];
    // The wire id is a UUID; Qdrant rejects "doc_0" outright.
    CHECK(p["id"] == detail::uuid_from_string("doc_0"));
    CHECK(p["id"] != "doc_0");
    CHECK(p["vector"] == json::array({1.0f, 2.0f}));
    CHECK(p["payload"]["content"] == "hello");
    CHECK(p["payload"]["tiny_agent_id"] == "doc_0");
    CHECK(p["payload"]["metadata"]["src"] == "test");
}

TEST_CASE("Qdrant reads hits from either endpoint shape") {
    json query_shape = {{"result", {{"points", json::array({
        {{"id", "uuid-1"}, {"score", 0.92},
         {"payload", {{"content", "hello"}, {"tiny_agent_id", "doc_0"},
                      {"metadata", {{"src", "test"}}}}}}
    })}}}};
    auto hits = QdrantVectorStore::parse_query_response(query_shape);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "doc_0");             // the id we stored, not the UUID
    CHECK(hits[0].content == "hello");
    CHECK(hits[0].score == doctest::Approx(0.92f));
    CHECK(hits[0].metadata["src"] == "test");

    json search_shape = {{"result", json::array({
        {{"id", 7}, {"score", 0.5}, {"payload", {{"content", "legacy"}}}}
    })}};
    auto legacy = QdrantVectorStore::parse_query_response(search_shape);
    REQUIRE(legacy.size() == 1);
    CHECK(legacy[0].id == "7");                // numeric ids fall back cleanly
    CHECK(legacy[0].content == "legacy");
}

TEST_CASE("Qdrant reports an unrecognised response instead of crashing") {
    CHECK_THROWS_AS(QdrantVectorStore::parse_query_response(json{{"status", "ok"}}), Error);
}

TEST_CASE("Qdrant rejects an empty collection name") {
    CHECK_THROWS_AS(QdrantVectorStore("http://localhost:6333", ""), Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Chroma — payload shaping, offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Chroma upsert body uses the parallel-array shape") {
    auto body = ChromaVectorStore::build_upsert_body({
        {"a", "alpha", {1.0f, 0.0f}, json{{"tag", "x"}}},
        {"b", "beta",  {0.0f, 1.0f}, json::object()}});

    CHECK(body["ids"] == json::array({"a", "b"}));
    CHECK(body["documents"] == json::array({"alpha", "beta"}));
    CHECK(body["embeddings"].size() == 2);
    CHECK(body["embeddings"][0] == json::array({1.0f, 0.0f}));
    CHECK(body["metadatas"][0]["tag"] == "x");
}

TEST_CASE("Chroma metadata is flattened to scalars") {
    auto flat = ChromaVectorStore::flatten_metadata(
        json{{"s", "text"}, {"n", 3}, {"b", true},
             {"nested", {{"deep", 1}}}, {"arr", json::array({1, 2})},
             {"nothing", nullptr}});

    CHECK(flat["s"] == "text");
    CHECK(flat["n"] == 3);
    CHECK(flat["b"] == true);
    // Chroma rejects nested values, so they become their JSON text.
    CHECK(flat["nested"].is_string());
    CHECK(flat["nested"] == R"({"deep":1})");
    CHECK(flat["arr"] == "[1,2]");
    CHECK_FALSE(flat.contains("nothing"));
}

TEST_CASE("Chroma query response unwraps one query and inverts distance") {
    json response = {
        {"ids",       json::array({json::array({"a", "b"})})},
        {"documents", json::array({json::array({"alpha", "beta"})})},
        {"metadatas", json::array({json::array({json{{"tag", "x"}}, json::object()})})},
        {"distances", json::array({json::array({0.05, 0.40})})}};

    auto hits = ChromaVectorStore::parse_query_response(response);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "a");
    CHECK(hits[0].content == "alpha");
    // Chroma reports distance (lower is closer); tiny_agent returns similarity.
    CHECK(hits[0].score == doctest::Approx(0.95f));
    CHECK(hits[1].score == doctest::Approx(0.60f));
    CHECK(hits[0].score > hits[1].score);
    CHECK(hits[0].metadata["tag"] == "x");
}

TEST_CASE("Chroma tolerates a response missing the optional arrays") {
    json response = {{"ids", json::array({json::array({"a"})})}};
    auto hits = ChromaVectorStore::parse_query_response(response);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "a");
    CHECK(hits[0].content.empty());
}

TEST_CASE("Chroma rejects an empty collection name") {
    CHECK_THROWS_AS(ChromaVectorStore("http://localhost:8000", ""), Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Live round-trips — skipped unless a server is configured
// ═══════════════════════════════════════════════════════════════════════════

// Exercises a store end to end: write three documents, search, count, drop.
template<typename Store>
static void round_trip(Store& store) {
    store.add_batch({
        {"doc_a", "the cat sat on the mat", {1.0f, 0.0f, 0.0f}, json{{"topic", "pets"}}},
        {"doc_b", "the dog ran in the park", {0.0f, 1.0f, 0.0f}, json{{"topic", "pets"}}},
        {"doc_c", "quantum field theory",    {0.0f, 0.0f, 1.0f}, json{{"topic", "physics"}}}});

    CHECK(store.size() == 3);

    auto hits = store.search({1.0f, 0.0f, 0.0f}, 2);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "doc_a");
    CHECK(hits[0].content == "the cat sat on the mat");
    CHECK(hits[0].metadata["topic"] == "pets");
    CHECK(hits[0].score > hits[1].score);   // similarity: best first

    // Re-adding the same id updates rather than duplicating.
    store.add("doc_a", "the cat sat on the mat", {1.0f, 0.0f, 0.0f}, json{{"topic", "pets"}});
    CHECK(store.size() == 3);

    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("live Qdrant round-trip") {
    const char* url = std::getenv("QDRANT_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    QdrantVectorStore store{url, "tiny_agent_test"};
    store.clear();
    round_trip(store);
}

TEST_CASE("live Chroma round-trip") {
    const char* url = std::getenv("CHROMA_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    ChromaVectorStore store{url, "tiny_agent_test"};
    store.clear();
    round_trip(store);
}

TEST_CASE("live Qdrant behind a Retriever") {
    const char* url = std::getenv("QDRANT_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    QdrantVectorStore store{url, "tiny_agent_retriever_test"};
    store.clear();

    Retriever<MockEmbed, QdrantVectorStore> r{std::move(store), MockEmbed{}, 2};
    r.add_documents({"the cat sat", "quantum mechanics"},
                    {json{{"topic", "pets"}}, json{{"topic", "physics"}}});

    auto hits = r.query("the cat sat", 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "the cat sat");
    r.store().clear();
}
