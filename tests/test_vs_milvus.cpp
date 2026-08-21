#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/vectorstore/base.hpp>
#include <tiny_agent/vectorstore/flat.hpp>
#include <tiny_agent/vectorstore/milvus.hpp>
#include <tiny_agent/retriever.hpp>
#include "mock_model.hpp"
#include <cstdlib>

using namespace tiny_agent;
using tiny_agent::test::MockEmbed;

// ═══════════════════════════════════════════════════════════════════════════
// Construction — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Milvus rejects an empty collection name") {
    CHECK_THROWS_AS(MilvusVectorStore("http://localhost:19530", ""), Error);
}

TEST_CASE("Milvus keeps the collection name it was given") {
    // Unlike Weaviate, nothing rewrites the name on the way to the server.
    MilvusVectorStore store{"http://localhost:19530", "tiny_agent_test"};
    CHECK(store.collection() == "tiny_agent_test");
}

// ═══════════════════════════════════════════════════════════════════════════
// Wire format — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Milvus schema declares a VarChar key so caller ids survive") {
    auto body = MilvusVectorStore::build_create_body(
        "docs", 3, "COSINE", "AUTOINDEX", 512, 65535);

    CHECK(body["collectionName"] == "docs");
    CHECK(body["schema"]["autoId"] == false);

    const auto& fields = body["schema"]["fields"];
    REQUIRE(fields.size() == 4);
    // Milvus's quick setup gives an Int64 key, which would force the adapter to
    // map every string id onto a number.
    CHECK(fields[0]["fieldName"] == "id");
    CHECK(fields[0]["dataType"] == "VarChar");
    CHECK(fields[0]["isPrimary"] == true);
    CHECK(fields[0]["elementTypeParams"]["max_length"] == "512");

    CHECK(fields[1]["fieldName"] == "vector");
    CHECK(fields[1]["dataType"] == "FloatVector");
    CHECK(fields[1]["elementTypeParams"]["dim"] == "3");

    CHECK(fields[2]["fieldName"] == "content");
    CHECK(fields[3]["fieldName"] == "metadata");
    CHECK(fields[3]["dataType"] == "JSON");
}

TEST_CASE("Milvus create carries the index, without which nothing loads") {
    auto body = MilvusVectorStore::build_create_body(
        "docs", 8, "COSINE", "AUTOINDEX", 512, 65535);

    REQUIRE(body["indexParams"].size() == 1);
    CHECK(body["indexParams"][0]["fieldName"] == "vector");
    CHECK(body["indexParams"][0]["metricType"] == "COSINE");
    CHECK(body["indexParams"][0]["indexType"] == "AUTOINDEX");

    // Quick setup defaults to Bounded, where a search can miss a write it
    // followed by milliseconds.
    CHECK(body["params"]["consistencyLevel"] == "Strong");
}

TEST_CASE("Milvus honours a metric and index other than the defaults") {
    auto body = MilvusVectorStore::build_create_body(
        "docs", 4, "L2", "IVF_FLAT", 64, 1024);

    CHECK(body["indexParams"][0]["metricType"] == "L2");
    CHECK(body["indexParams"][0]["indexType"] == "IVF_FLAT");
    CHECK(body["schema"]["fields"][0]["elementTypeParams"]["max_length"] == "64");
    CHECK(body["schema"]["fields"][2]["elementTypeParams"]["max_length"] == "1024");
}

TEST_CASE("Milvus writes the caller's id straight into the primary key") {
    auto body = MilvusVectorStore::build_upsert_body(
        "docs", {{"doc_0", "hello", {1.0f, 2.0f}, json{{"src", "test"}}}});

    CHECK(body["collectionName"] == "docs");
    REQUIRE(body["data"].size() == 1);
    auto& row = body["data"][0];
    // Qdrant and Weaviate both need a UUID here; a VarChar key takes the id
    // itself, so there is nothing to hash and nothing to carry alongside.
    CHECK(row["id"] == "doc_0");
    CHECK(row["vector"] == json::array({1.0f, 2.0f}));
    CHECK(row["content"] == "hello");
    CHECK(row["metadata"] == json{{"src", "test"}});
    CHECK_FALSE(row.contains("tiny_agent_id"));
}

TEST_CASE("Milvus batches every document into one request") {
    auto body = MilvusVectorStore::build_upsert_body("docs", {
        {"a", "alpha", {1.0f, 0.0f}, json::object()},
        {"b", "beta",  {0.0f, 1.0f}, json::object()},
        {"c", "gamma", {1.0f, 1.0f}, json::object()}});

    REQUIRE(body["data"].size() == 3);
    CHECK(body["data"][0]["id"] == "a");
    CHECK(body["data"][2]["id"] == "c");
}

TEST_CASE("Milvus keeps nested metadata as an object rather than text") {
    json meta = {{"topic", "pets"}, {"nested", {{"deep", 1}}}, {"arr", json::array({1, 2})}};
    auto body = MilvusVectorStore::build_upsert_body("docs", {{"a", "x", {1.0f}, meta}});

    // Chroma flattens nested values to their JSON text and Weaviate stores the
    // whole object as one text property. A Milvus JSON field takes the object.
    CHECK(body["data"][0]["metadata"] == meta);
    CHECK(body["data"][0]["metadata"]["nested"]["deep"] == 1);
}

TEST_CASE("Milvus sends an empty object where there is no metadata") {
    auto body = MilvusVectorStore::build_upsert_body("docs", {{"a", "x", {1.0f}, json()}});
    CHECK(body["data"][0]["metadata"] == json::object());
}

TEST_CASE("Milvus search asks for one query vector and names the output fields") {
    auto body = MilvusVectorStore::build_search_body("docs", {1.0f, 0.0f}, 3);

    CHECK(body["collectionName"] == "docs");
    // "data" is a list of query vectors; this interface sends exactly one.
    CHECK(body["data"] == json::array({json::array({1.0f, 0.0f})}));
    CHECK(body["annsField"] == "vector");
    CHECK(body["limit"] == 3);
    CHECK(body["outputFields"] == json::array({"id", "content", "metadata"}));

    // A limit of zero is a request Milvus refuses and a caller never wants.
    CHECK(MilvusVectorStore::build_search_body("docs", {1.0f}, 0)["limit"] == 1);
    CHECK(MilvusVectorStore::build_search_body("docs", {1.0f}, -5)["limit"] == 1);
}

TEST_CASE("Milvus counts with an empty filter and count(*)") {
    auto body = MilvusVectorStore::build_count_body("docs");
    CHECK(body["collectionName"] == "docs");
    CHECK(body["filter"] == "");
    CHECK(body["outputFields"] == json::array({"count(*)"}));
}

// ═══════════════════════════════════════════════════════════════════════════
// Failures that arrive as a success — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Milvus reports refusals in the body, so the body is what is checked") {
    CHECK_NOTHROW(MilvusVectorStore::check_code(json{{"code", 0}, {"data", json::object()}}, "add"));
    // No "code" at all is the shape of a response that carried no failure.
    CHECK_NOTHROW(MilvusVectorStore::check_code(json{{"data", json::object()}}, "add"));

    // This one arrives HTTP 200. Only the body says the write did not happen.
    json dim_mismatch = {{"code", 1804},
        {"message", "fail to deal the insert data, error: []float32 size 2 doesn't "
                    "equal to vector dimension 3 of FloatVector: invalid parameter"}};
    CHECK_THROWS_AS(MilvusVectorStore::check_code(dim_mismatch, "add"), Error);

    json missing = {{"code", 100},
                    {"message", "can't find collection[database=default][collection=nope]"}};
    CHECK_THROWS_AS(MilvusVectorStore::check_code(missing, "search"), Error);

    json not_loaded = {{"code", 101},
                       {"message", "failed to search: collection not loaded"}};
    CHECK_THROWS_AS(MilvusVectorStore::check_code(not_loaded, "search"), Error);
}

TEST_CASE("Milvus error messages keep the server's own words") {
    json err = {{"code", 1804}, {"message", "vector dimension 3 of FloatVector"}};
    try {
        MilvusVectorStore::check_code(err, "add");
        FAIL("expected a throw");
    } catch (const Error& e) {
        std::string msg = e.what();
        CHECK(msg.find("1804") != std::string::npos);
        CHECK(msg.find("vector dimension 3") != std::string::npos);
        CHECK(msg.find("MilvusVectorStore::add") != std::string::npos);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Reading a response — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Milvus hands back the id that was stored, no mapping in between") {
    json response = {{"code", 0}, {"data", json::array({
        json{{"id", "doc_a"}, {"content", "alpha"}, {"distance", 0.97},
             {"metadata", R"({"topic":"pets"})"}},
        json{{"id", "doc_b"}, {"content", "beta"}, {"distance", 0.65},
             {"metadata", "{}"}}})}};

    auto hits = MilvusVectorStore::parse_search_response(response, "COSINE");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "doc_a");
    CHECK(hits[0].content == "alpha");
    CHECK(hits[0].metadata["topic"] == "pets");
    CHECK(hits[0].score > hits[1].score);
    CHECK(hits[1].metadata == json::object());
}

TEST_CASE("Milvus cosine distance is already a similarity") {
    // Chroma and Weaviate report a real distance and their adapters invert it.
    // Milvus reports the cosine similarity under the name "distance".
    CHECK(MilvusVectorStore::to_similarity("COSINE", 0.97f) == doctest::Approx(0.97f));
    CHECK(MilvusVectorStore::to_similarity("IP", 4.2f)      == doctest::Approx(4.2f));
    // L2 really is a distance, so it is negated to keep "higher is closer".
    CHECK(MilvusVectorStore::to_similarity("L2", 0.25f)     == doctest::Approx(-0.25f));
    CHECK(MilvusVectorStore::to_similarity("L2", 4.0f)
          < MilvusVectorStore::to_similarity("L2", 0.5f));
}

TEST_CASE("Milvus L2 hits still come back best first") {
    json response = {{"code", 0}, {"data", json::array({
        json{{"id", "near"}, {"content", "a"}, {"distance", 0.10}, {"metadata", "{}"}},
        json{{"id", "far"},  {"content", "b"}, {"distance", 9.00}, {"metadata", "{}"}}})}};

    auto hits = MilvusVectorStore::parse_search_response(response, "L2");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "near");
    CHECK(hits[0].score > hits[1].score);
}

TEST_CASE("Milvus metadata comes back as text over REST and is parsed back") {
    // The field is JSON on the server; the REST response serialises it.
    json meta = {{"topic", "pets"}, {"nested", {{"deep", 1}}}, {"arr", json::array({1, 2})}};
    CHECK(MilvusVectorStore::parse_metadata(json(meta.dump())) == meta);
    // An object, should a future server version send one, passes straight through.
    CHECK(MilvusVectorStore::parse_metadata(meta) == meta);
}

TEST_CASE("Milvus treats absent or unparseable metadata as none") {
    CHECK(MilvusVectorStore::parse_metadata(json()) == json::object());
    CHECK(MilvusVectorStore::parse_metadata(json("")) == json::object());
    CHECK(MilvusVectorStore::parse_metadata(json("not json")) == json::object());
    CHECK(MilvusVectorStore::parse_metadata(json("[1,2]")) == json::object());
    CHECK(MilvusVectorStore::parse_metadata(json(42)) == json::object());
}

TEST_CASE("Milvus tolerates a hit missing its optional fields") {
    json response = {{"code", 0}, {"data", json::array({json{{"id", "bare"}}})}};
    auto hits = MilvusVectorStore::parse_search_response(response, "COSINE");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "bare");
    CHECK(hits[0].content.empty());
    CHECK(hits[0].score == doctest::Approx(0.0f));
    CHECK(hits[0].metadata == json::object());
}

TEST_CASE("Milvus surfaces a failed search instead of an empty result") {
    json failed = {{"code", 101}, {"message", "collection not loaded"}};
    CHECK_THROWS_AS(MilvusVectorStore::parse_search_response(failed, "COSINE"), Error);

    // Success with a shape nobody expects is also an error, not zero hits.
    CHECK_THROWS_AS(
        MilvusVectorStore::parse_search_response(json{{"code", 0}, {"data", 7}}, "COSINE"),
        Error);
    CHECK_THROWS_AS(
        MilvusVectorStore::parse_search_response(json{{"code", 0}}, "COSINE"), Error);
}

TEST_CASE("Milvus reads an empty result as no hits") {
    json empty = {{"code", 0}, {"data", json::array()}};
    CHECK(MilvusVectorStore::parse_search_response(empty, "COSINE").empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// It is a vector store like the others — offline
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Milvus satisfies both store concepts") {
    static_assert(vector_store<MilvusVectorStore>);
    static_assert(batch_vector_store<MilvusVectorStore>);
}

TEST_CASE("Milvus fits inside AnyVectorStore") {
    // Constructing the wrapper talks to nobody; the first request would.
    AnyVectorStore store = MilvusVectorStore{"http://localhost:19530", "tiny_agent_test"};
    static_assert(vector_store<AnyVectorStore>);
    CHECK(true);
}

TEST_CASE("Milvus scores the way FlatVectorStore does") {
    // COSINE reports the cosine similarity itself, which is exactly what
    // FlatVectorStore computes, so the two rank alike and score alike with no
    // conversion anywhere in between.
    FlatVectorStore flat;
    flat.add("a", "alpha", {1.0f, 0.0f, 0.0f}, json::object());
    flat.add("b", "beta",  {0.0f, 1.0f, 0.0f}, json::object());

    auto flat_hits = flat.search({1.0f, 0.0f, 0.0f}, 2);

    json as_milvus = {{"code", 0}, {"data", json::array({
        json{{"id", "a"}, {"content", "alpha"}, {"distance", 1.0}, {"metadata", "{}"}},
        json{{"id", "b"}, {"content", "beta"},  {"distance", 0.0}, {"metadata", "{}"}}})}};
    auto milvus_hits = MilvusVectorStore::parse_search_response(as_milvus, "COSINE");

    REQUIRE(milvus_hits.size() == flat_hits.size());
    for (std::size_t i = 0; i < flat_hits.size(); ++i) {
        CHECK(milvus_hits[i].id == flat_hits[i].id);
        CHECK(milvus_hits[i].score == doctest::Approx(flat_hits[i].score));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Live round-trips — skipped unless MILVUS_URL is set
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("live Milvus round-trip") {
    const char* url = std::getenv("MILVUS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    MilvusVectorStore store{url, "tiny_agent_test"};
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

    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("live Milvus updates on a repeated id instead of duplicating") {
    const char* url = std::getenv("MILVUS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    // /entities/insert would grow the collection on every re-index; the adapter
    // upserts, so the row count holds and the content is the newer one.
    MilvusVectorStore store{url, "tiny_agent_upsert_test"};
    store.clear();

    store.add("doc_a", "first version", {1.0f, 0.0f}, json{{"v", 1}});
    CHECK(store.size() == 1);

    store.add("doc_a", "second version", {1.0f, 0.0f}, json{{"v", 2}});
    CHECK(store.size() == 1);

    auto hits = store.search({1.0f, 0.0f}, 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "second version");
    CHECK(hits[0].metadata["v"] == 2);

    store.clear();
}

TEST_CASE("live Milvus keeps nested metadata intact") {
    const char* url = std::getenv("MILVUS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    MilvusVectorStore store{url, "tiny_agent_meta_test"};
    store.clear();

    json meta = {{"topic", "pets"}, {"nested", {{"deep", 1}}}, {"arr", json::array({1, 2})}};
    store.add("doc_a", "the cat sat", {1.0f, 0.0f}, meta);

    auto hits = store.search({1.0f, 0.0f}, 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].metadata == meta);
    store.clear();
}

TEST_CASE("live Milvus ranks the same way FlatVectorStore does") {
    const char* url = std::getenv("MILVUS_URL");
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

    MilvusVectorStore store{url, "tiny_agent_rank_test"};
    store.clear();
    store.add_batch(corpus);

    const std::vector<float> query = {0.9f, 0.4f, 0.1f};
    auto flat_hits   = flat.search(query, 3);
    auto milvus_hits = store.search(query, 3);

    REQUIRE(milvus_hits.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(milvus_hits[i].id == flat_hits[i].id);
        CHECK(milvus_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001));
    }
    store.clear();
}

TEST_CASE("live Milvus refuses a vector of the wrong width") {
    const char* url = std::getenv("MILVUS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    // The refusal arrives HTTP 200. If the adapter read only the status this
    // write would look like it landed.
    MilvusVectorStore store{url, "tiny_agent_dim_test"};
    store.clear();
    store.add("doc_a", "three wide", {1.0f, 0.0f, 0.0f}, json::object());

    CHECK_THROWS_AS(store.add("doc_b", "two wide", {1.0f, 0.0f}, json::object()), Error);
    CHECK(store.size() == 1);
    store.clear();
}

TEST_CASE("live Milvus behind a Retriever") {
    const char* url = std::getenv("MILVUS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    MilvusVectorStore store{url, "tiny_agent_retriever_test"};
    store.clear();

    Retriever<MockEmbed, MilvusVectorStore> r{std::move(store), MockEmbed{}, 2};
    r.add_documents({"the cat sat", "quantum mechanics"},
                    {json{{"topic", "pets"}}, json{{"topic", "physics"}}});

    auto hits = r.query("the cat sat", 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "the cat sat");
    CHECK(hits[0].metadata["topic"] == "pets");
    r.store().clear();
}

TEST_CASE("live Milvus through AnyVectorStore") {
    const char* url = std::getenv("MILVUS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    AnyVectorStore store = MilvusVectorStore{url, "tiny_agent_any_test"};
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
