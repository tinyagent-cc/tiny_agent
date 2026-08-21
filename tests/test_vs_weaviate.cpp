#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/vectorstore/base.hpp>
#include <tiny_agent/vectorstore/flat.hpp>
#include <tiny_agent/vectorstore/weaviate.hpp>
#include <tiny_agent/retriever.hpp>
#include "mock_model.hpp"
#include <cstdlib>

using namespace tiny_agent;
using tiny_agent::test::MockEmbed;

// ═══════════════════════════════════════════════════════════════════════════
// Collection naming — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Weaviate capitalises the collection name the way the server does") {
    // Weaviate stores "tiny_agent_test" as "Tiny_agent_test" and then answers
    // 404 on every path spelled the way it was asked for.
    CHECK(detail::weaviate_class_name("tiny_agent_test") == "Tiny_agent_test");
    CHECK(detail::weaviate_class_name("MyDocs")          == "MyDocs");
    CHECK(detail::weaviate_class_name("d")               == "D");
}

TEST_CASE("Weaviate rejects a name GraphQL cannot express") {
    CHECK_THROWS_AS(detail::weaviate_class_name(""),          Error);
    CHECK_THROWS_AS(detail::weaviate_class_name("9docs"),     Error);
    CHECK_THROWS_AS(detail::weaviate_class_name("my-docs"),   Error);
    CHECK_THROWS_AS(detail::weaviate_class_name("my docs"),   Error);
    CHECK_THROWS_AS(detail::weaviate_class_name("docs.v2"),   Error);
}

TEST_CASE("Weaviate rejects an empty collection name") {
    CHECK_THROWS_AS(WeaviateVectorStore("http://localhost:8080", ""), Error);
}

TEST_CASE("Weaviate reports the name the server holds, not the one it was given") {
    WeaviateVectorStore store{"http://localhost:8080", "tiny_agent_test"};
    CHECK(store.collection() == "Tiny_agent_test");
}

// ═══════════════════════════════════════════════════════════════════════════
// Wire format — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Weaviate schema declares vectorizer none and no dimension") {
    auto body = WeaviateVectorStore::build_schema_body("Docs", "cosine");

    CHECK(body["class"] == "Docs");
    // Without this the server embeds the text itself with whatever
    // DEFAULT_VECTORIZER_MODULE it was started with.
    CHECK(body["vectorizer"] == "none");
    CHECK(body["vectorIndexConfig"]["distance"] == "cosine");
    // Unlike Qdrant, Weaviate sizes the index from the first object written.
    CHECK_FALSE(body["vectorIndexConfig"].contains("size"));

    REQUIRE(body["properties"].size() == 3);
    CHECK(body["properties"][0]["name"] == "content");
    CHECK(body["properties"][1]["name"] == "tiny_agent_id");
    CHECK(body["properties"][2]["name"] == "metadata");
    CHECK(body["properties"][0]["dataType"] == json::array({"text"}));
}

TEST_CASE("Weaviate object ids are UUIDs, deterministically derived") {
    auto a = detail::uuid_from_string("doc_0");
    auto b = detail::uuid_from_string("doc_0");
    auto c = detail::uuid_from_string("doc_1");

    CHECK(a == b);                     // same input, same object
    CHECK(a != c);
    CHECK(a.size() == 36);
    CHECK(a.find_first_not_of("0123456789abcdef-") == std::string::npos);
}

TEST_CASE("Weaviate batch body carries the original id in a property") {
    auto body = WeaviateVectorStore::build_batch_body(
        "Docs", {{"doc_0", "hello", {1.0f, 2.0f}, json{{"src", "test"}}}});

    REQUIRE(body["objects"].size() == 1);
    auto& o = body["objects"][0];
    CHECK(o["class"] == "Docs");                 // each object names its collection
    // The wire id is a UUID; Weaviate rejects "doc_0" outright.
    CHECK(o["id"] == detail::uuid_from_string("doc_0"));
    CHECK(o["id"] != "doc_0");
    CHECK(o["vector"] == json::array({1.0f, 2.0f}));
    CHECK(o["properties"]["content"] == "hello");
    CHECK(o["properties"]["tiny_agent_id"] == "doc_0");
    CHECK(o["properties"]["metadata"] == R"({"src":"test"})");
}

TEST_CASE("Weaviate metadata round-trips as an object, nesting and all") {
    json meta = {{"topic", "pets"}, {"nested", {{"deep", 1}}}, {"arr", json::array({1, 2})}};
    auto body = WeaviateVectorStore::build_batch_body("Docs", {{"a", "x", {1.0f}, meta}});
    auto back = WeaviateVectorStore::parse_metadata(
        body["objects"][0]["properties"]["metadata"].get<std::string>());

    // Chroma flattens nested values to their JSON text; here nothing flattens.
    CHECK(back == meta);
    CHECK(back["nested"]["deep"] == 1);
    CHECK(back["arr"] == json::array({1, 2}));
}

TEST_CASE("Weaviate treats absent or unparseable metadata as none") {
    CHECK(WeaviateVectorStore::parse_metadata("") == json::object());
    CHECK(WeaviateVectorStore::parse_metadata("not json") == json::object());
    CHECK(WeaviateVectorStore::parse_metadata("[1,2]") == json::object());

    auto body = WeaviateVectorStore::build_batch_body("Docs", {{"a", "x", {1.0f}, json()}});
    CHECK(body["objects"][0]["properties"]["metadata"] == "{}");
}

TEST_CASE("Weaviate search is a GraphQL query, not a JSON body") {
    auto q = WeaviateVectorStore::build_search_query("Docs", {1.0f, 0.0f}, 3);

    CHECK(q.find("Get { Docs(") != std::string::npos);
    CHECK(q.find("nearVector: {vector: [1.0,0.0]}") != std::string::npos);
    CHECK(q.find("limit: 3") != std::string::npos);
    CHECK(q.find("_additional { id distance }") != std::string::npos);
    CHECK(q.find("tiny_agent_id") != std::string::npos);

    // A limit of zero is a query Weaviate accepts and a caller never wants.
    CHECK(WeaviateVectorStore::build_search_query("Docs", {1.0f}, 0)
              .find("limit: 1") != std::string::npos);
}

TEST_CASE("Weaviate batch failures hide behind a 200, so the body is checked") {
    json all_good = json::array({
        json{{"id", "uuid-1"}, {"result", {{"status", "SUCCESS"}}}}});
    CHECK_NOTHROW(WeaviateVectorStore::check_batch_response(all_good));

    // The endpoint answers 200 for this. Only the per-object result says so.
    json one_failed = json::array({
        json{{"id", "uuid-1"}, {"result", {{"status", "SUCCESS"}}}},
        json{{"id", "uuid-2"}, {"result", {{"status", "FAILED"},
             {"errors", {{"error", json::array({json{{"message",
                 "vector dimensions do not match the index dimensions"}}})}}}}}}});
    CHECK_THROWS_AS(WeaviateVectorStore::check_batch_response(one_failed), Error);

    CHECK_THROWS_AS(WeaviateVectorStore::check_batch_response(json{{"error", "x"}}), Error);
}

TEST_CASE("Weaviate query response inverts distance into a similarity") {
    json response = {{"data", {{"Get", {{"Docs", json::array({
        json{{"content", "alpha"}, {"tiny_agent_id", "a"},
             {"metadata", R"({"tag":"x"})"},
             {"_additional", {{"id", "uuid-1"}, {"distance", 0.05}}}},
        json{{"content", "beta"}, {"tiny_agent_id", "b"},
             {"metadata", "{}"},
             {"_additional", {{"id", "uuid-2"}, {"distance", 0.40}}}}
    })}}}}}};

    auto hits = WeaviateVectorStore::parse_query_response(response, "Docs");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "a");                 // the id we stored, not the UUID
    CHECK(hits[0].content == "alpha");
    // Weaviate reports distance (lower is closer); tiny_agent returns similarity.
    CHECK(hits[0].score == doctest::Approx(0.95f));
    CHECK(hits[1].score == doctest::Approx(0.60f));
    CHECK(hits[0].score > hits[1].score);
    CHECK(hits[0].metadata["tag"] == "x");
    CHECK(hits[1].metadata == json::object());
}

TEST_CASE("Weaviate falls back to the UUID when the stored id is missing") {
    json response = {{"data", {{"Get", {{"Docs", json::array({
        json{{"content", "orphan"},
             {"_additional", {{"id", "uuid-1"}, {"distance", 0.0}}}}
    })}}}}}};

    auto hits = WeaviateVectorStore::parse_query_response(response, "Docs");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "uuid-1");
    CHECK(hits[0].score == doctest::Approx(1.0f));
}

TEST_CASE("Weaviate surfaces a GraphQL error instead of an empty result") {
    // GraphQL answers 200 whatever happened; the failure is in the body.
    json failed = {{"data", {{"Get", {{"Docs", nullptr}}}}},
                   {"errors", json::array({json{{"message",
                       "vector lengths don't match"}}})}};
    CHECK_THROWS_AS(WeaviateVectorStore::parse_query_response(failed, "Docs"), Error);

    CHECK_THROWS_AS(WeaviateVectorStore::parse_query_response(json{{"ok", true}}, "Docs"), Error);

    json wrong_class = {{"data", {{"Get", {{"Other", json::array()}}}}}};
    CHECK_THROWS_AS(WeaviateVectorStore::parse_query_response(wrong_class, "Docs"), Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// It is a vector store like the others — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Weaviate satisfies both store concepts") {
    static_assert(vector_store<WeaviateVectorStore>);
    static_assert(batch_vector_store<WeaviateVectorStore>);
}

TEST_CASE("Weaviate fits inside AnyVectorStore") {
    // Constructing the wrapper talks to nobody; the first request would.
    AnyVectorStore store = WeaviateVectorStore{"http://localhost:8080", "tiny_agent_test"};
    static_assert(vector_store<AnyVectorStore>);
    CHECK(true);
}

TEST_CASE("Weaviate scores the way FlatVectorStore does") {
    // Cosine distance is 1 - cosine similarity, so 1 - distance is exactly what
    // FlatVectorStore computes. The two stores rank alike and score alike.
    FlatVectorStore flat;
    flat.add("a", "alpha", {1.0f, 0.0f, 0.0f}, json::object());
    flat.add("b", "beta",  {0.0f, 1.0f, 0.0f}, json::object());

    auto flat_hits = flat.search({1.0f, 0.0f, 0.0f}, 2);

    json as_weaviate = {{"data", {{"Get", {{"Docs", json::array({
        json{{"content", "alpha"}, {"tiny_agent_id", "a"}, {"metadata", "{}"},
             {"_additional", {{"id", "u1"}, {"distance", 0.0}}}},   // cos sim 1.0
        json{{"content", "beta"}, {"tiny_agent_id", "b"}, {"metadata", "{}"},
             {"_additional", {{"id", "u2"}, {"distance", 1.0}}}}    // cos sim 0.0
    })}}}}}};
    auto weaviate_hits = WeaviateVectorStore::parse_query_response(as_weaviate, "Docs");

    REQUIRE(weaviate_hits.size() == flat_hits.size());
    for (std::size_t i = 0; i < flat_hits.size(); ++i) {
        CHECK(weaviate_hits[i].id == flat_hits[i].id);
        CHECK(weaviate_hits[i].score == doctest::Approx(flat_hits[i].score));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Live round-trips — skipped unless WEAVIATE_URL is set
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("live Weaviate round-trip") {
    const char* url = std::getenv("WEAVIATE_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    WeaviateVectorStore store{url, "tiny_agent_test"};
    store.clear();

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
    CHECK(hits[0].score == doctest::Approx(1.0f).epsilon(0.001));

    // Re-adding the same id updates rather than duplicating.
    store.add("doc_a", "the cat sat on the mat", {1.0f, 0.0f, 0.0f}, json{{"topic", "pets"}});
    CHECK(store.size() == 3);

    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("live Weaviate keeps nested metadata intact") {
    const char* url = std::getenv("WEAVIATE_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    WeaviateVectorStore store{url, "tiny_agent_meta_test"};
    store.clear();

    json meta = {{"topic", "pets"}, {"nested", {{"deep", 1}}}, {"arr", json::array({1, 2})}};
    store.add("doc_a", "the cat sat", {1.0f, 0.0f}, meta);

    auto hits = store.search({1.0f, 0.0f}, 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].metadata == meta);
    store.clear();
}

TEST_CASE("live Weaviate ranks the same way FlatVectorStore does") {
    const char* url = std::getenv("WEAVIATE_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    const std::vector<Document> corpus = {
        {"doc_a", "the cat sat on the mat", {1.0f, 0.2f, 0.0f}, json{{"topic", "pets"}}},
        {"doc_b", "the dog ran in the park", {0.3f, 1.0f, 0.1f}, json{{"topic", "pets"}}},
        {"doc_c", "quantum field theory",    {0.0f, 0.1f, 1.0f}, json{{"topic", "physics"}}}};

    FlatVectorStore flat;
    add_documents(flat, corpus);

    WeaviateVectorStore store{url, "tiny_agent_rank_test"};
    store.clear();
    store.add_batch(corpus);

    const std::vector<float> query = {0.9f, 0.4f, 0.1f};
    auto flat_hits     = flat.search(query, 3);
    auto weaviate_hits = store.search(query, 3);

    REQUIRE(weaviate_hits.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(weaviate_hits[i].id == flat_hits[i].id);
        CHECK(weaviate_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001));
    }
    store.clear();
}

TEST_CASE("live Weaviate behind a Retriever") {
    const char* url = std::getenv("WEAVIATE_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    WeaviateVectorStore store{url, "tiny_agent_retriever_test"};
    store.clear();

    Retriever<MockEmbed, WeaviateVectorStore> r{std::move(store), MockEmbed{}, 2};
    r.add_documents({"the cat sat", "quantum mechanics"},
                    {json{{"topic", "pets"}}, json{{"topic", "physics"}}});

    auto hits = r.query("the cat sat", 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "the cat sat");
    CHECK(hits[0].metadata["topic"] == "pets");
    r.store().clear();
}

TEST_CASE("live Weaviate through AnyVectorStore") {
    const char* url = std::getenv("WEAVIATE_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    AnyVectorStore store = WeaviateVectorStore{url, "tiny_agent_any_test"};
    store.clear();
    store.add_batch({{"a", "alpha", {1.0f, 0.0f}, json::object()},
                     {"b", "beta",  {0.0f, 1.0f}, json::object()}});
    CHECK(store.size() == 2);

    auto hits = store.search({1.0f, 0.0f}, 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "a");
    store.clear();
    CHECK(store.size() == 0);
}
