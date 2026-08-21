#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/vectorstore/base.hpp>
#include <tiny_agent/vectorstore/flat.hpp>
#include <tiny_agent/vectorstore/redis.hpp>
#include <tiny_agent/retriever.hpp>
#include "mock_model.hpp"
#include <bit>
#include <cstdlib>
#include <cstring>

using namespace tiny_agent;
using tiny_agent::test::MockEmbed;
namespace resp = tiny_agent::detail::redis;
using Reply = resp::Reply;

// ═══════════════════════════════════════════════════════════════════════════
// RESP2 encoding
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("a command encodes as an array of bulk strings") {
    CHECK(resp::encode_command({"PING"}) == "*1\r\n$4\r\nPING\r\n");
    CHECK(resp::encode_command({"HSET", "k", "f", "v"})
          == "*4\r\n$4\r\nHSET\r\n$1\r\nk\r\n$1\r\nf\r\n$1\r\nv\r\n");
}

TEST_CASE("bulk strings are length-prefixed, so binary arguments need no escaping") {
    // A float32 blob contains NULs, CRs and LFs. The length header is what
    // makes that safe; nothing in the encoder looks at the bytes.
    std::string blob("\x00\x0d\x0a\x24", 4);
    auto        wire = resp::encode_command({"SET", "k", blob});
    CHECK(wire == std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\n\x00\x0d\x0a\x24\r\n", 30));
}

TEST_CASE("a pipeline is the commands back to back") {
    auto wire = resp::encode_pipeline({{"PING"}, {"PING"}});
    CHECK(wire == "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// RESP2 parsing
// ═══════════════════════════════════════════════════════════════════════════

static Reply parse_all(std::string_view wire) {
    Reply       reply;
    std::size_t consumed = 0;
    REQUIRE(resp::parse_reply(wire, reply, consumed));
    CHECK(consumed == wire.size());
    return reply;
}

TEST_CASE("the five RESP2 reply types round-trip") {
    auto simple = parse_all("+OK\r\n");
    CHECK(simple.type == Reply::Type::simple);
    CHECK(simple.str == "OK");

    auto err = parse_all("-ERR unknown command\r\n");
    CHECK(err.is_error());
    CHECK(err.str == "ERR unknown command");

    auto number = parse_all(":42\r\n");
    CHECK(number.type == Reply::Type::integer);
    CHECK(number.integer == 42);

    auto bulk = parse_all("$5\r\nhello\r\n");
    CHECK(bulk.type == Reply::Type::bulk);
    CHECK(bulk.str == "hello");

    auto array = parse_all("*2\r\n$1\r\na\r\n:7\r\n");
    CHECK(array.type == Reply::Type::array);
    REQUIRE(array.elements.size() == 2);
    CHECK(array.elements[0].str == "a");
    CHECK(array.elements[1].integer == 7);
}

TEST_CASE("a bulk string carries its bytes verbatim, NULs included") {
    auto bulk = parse_all(std::string_view("$4\r\n\x00\x0d\x0a\x24\r\n", 10));
    CHECK(bulk.str.size() == 4);
    CHECK(bulk.str[0] == '\0');
    CHECK(bulk.str[3] == '$');
}

TEST_CASE("null bulk and null array both read as null") {
    CHECK(parse_all("$-1\r\n").is_null());
    CHECK(parse_all("*-1\r\n").is_null());
}

TEST_CASE("nested arrays parse to depth") {
    auto reply = parse_all("*2\r\n:1\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    REQUIRE(reply.elements.size() == 2);
    CHECK(reply.elements[0].integer == 1);
    REQUIRE(reply.elements[1].elements.size() == 2);
    CHECK(reply.elements[1].elements[1].str == "b");
}

TEST_CASE("a partial reply asks for more bytes instead of guessing") {
    Reply       reply;
    std::size_t consumed = 0;

    CHECK_FALSE(resp::parse_reply("", reply, consumed));
    CHECK_FALSE(resp::parse_reply("+OK", reply, consumed));           // no terminator yet
    CHECK_FALSE(resp::parse_reply("$5\r\nhel", reply, consumed));     // body still arriving
    CHECK_FALSE(resp::parse_reply("$5\r\nhello\r", reply, consumed)); // trailing CRLF split
    CHECK_FALSE(resp::parse_reply("*2\r\n:1\r\n", reply, consumed));  // second element missing
}

TEST_CASE("only the first reply is consumed when two are buffered together") {
    Reply       reply;
    std::size_t consumed = 0;
    REQUIRE(resp::parse_reply("+OK\r\n:9\r\n", reply, consumed));
    CHECK(reply.str == "OK");
    CHECK(consumed == 5);
}

TEST_CASE("a byte that cannot start a reply is a desync, not a slow socket") {
    Reply       reply;
    std::size_t consumed = 0;
    CHECK_THROWS_AS(resp::parse_reply("?nonsense\r\n", reply, consumed), Error);
    CHECK_THROWS_AS(resp::parse_reply("$abc\r\n", reply, consumed), Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Connection strings
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("url parsing covers the forms people actually type") {
    auto plain = resp::parse_url("redis://localhost:6379");
    CHECK(plain.host == "localhost");
    CHECK(plain.port == 6379);
    CHECK(plain.db == 0);

    auto bare = resp::parse_url("127.0.0.1");
    CHECK(bare.host == "127.0.0.1");
    CHECK(bare.port == 6379);

    auto with_db = resp::parse_url("redis://cache:6380/3");
    CHECK(with_db.host == "cache");
    CHECK(with_db.port == 6380);
    CHECK(with_db.db == 3);

    auto password_only = resp::parse_url("redis://s3cret@cache:6379");
    CHECK(password_only.username.empty());
    CHECK(password_only.password == "s3cret");
    CHECK(password_only.host == "cache");

    auto acl = resp::parse_url("redis://alice:s3cret@cache:6379/1");
    CHECK(acl.username == "alice");
    CHECK(acl.password == "s3cret");
    CHECK(acl.db == 1);

    auto v6 = resp::parse_url("redis://[::1]:6379");
    CHECK(v6.host == "::1");
    CHECK(v6.port == 6379);
}

TEST_CASE("url parsing refuses what it cannot honour") {
    CHECK_THROWS_AS(resp::parse_url(""), Error);
    CHECK_THROWS_AS(resp::parse_url("redis://host:99999"), Error);
    // No TLS in this client, and saying so beats failing at the handshake.
    CHECK_THROWS_AS(resp::parse_url("rediss://host:6379"), Error);
}

TEST_CASE("Redis rejects an empty index name") {
    CHECK_THROWS_AS(RedisVectorStore("redis://localhost:6379", ""), Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Command format
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("vectors go out as raw little-endian float32") {
    auto blob = RedisVectorStore::encode_vector({1.0f, -2.0f});
    REQUIRE(blob.size() == 8);

    float back[2];
    std::memcpy(back, blob.data(), 8);
    // On a little-endian host the bytes are already in memory order; the
    // encoder writes LSB first, so this holds on a big-endian one too after
    // the same byte-order read.
    if constexpr (std::endian::native == std::endian::little) {
        CHECK(back[0] == doctest::Approx(1.0f));
        CHECK(back[1] == doctest::Approx(-2.0f));
    }
    CHECK(static_cast<unsigned char>(blob[0]) == 0x00);
    CHECK(static_cast<unsigned char>(blob[3]) == 0x3F);   // 1.0f is 3F800000
}

TEST_CASE("FT.CREATE indexes the vector field and nothing else") {
    auto cmd = RedisVectorStore::build_create_index_command(
        "docs", "docs:", 384, "HNSW", "COSINE");

    CHECK(cmd[0] == "FT.CREATE");
    CHECK(cmd[1] == "docs");
    CHECK(cmd[2] == "ON");
    CHECK(cmd[3] == "HASH");
    CHECK(cmd[4] == "PREFIX");
    CHECK(cmd[5] == "1");
    CHECK(cmd[6] == "docs:");
    CHECK(cmd[7] == "SCHEMA");
    CHECK(cmd[8] == "embedding");
    CHECK(cmd[9] == "VECTOR");
    CHECK(cmd[10] == "HNSW");
    // The attribute count RediSearch expects: TYPE, DIM and DISTANCE_METRIC,
    // name and value each counted.
    CHECK(cmd[11] == "6");
    CHECK(cmd[13] == "FLOAT32");
    CHECK(cmd[15] == "384");
    CHECK(cmd[17] == "COSINE");
    CHECK(cmd.size() == 18);

    auto flat = RedisVectorStore::build_create_index_command("docs", "docs:", 4, "FLAT", "L2");
    CHECK(flat[10] == "FLAT");
    CHECK(flat[17] == "L2");
}

TEST_CASE("HSET writes content, metadata and the vector under a prefixed key") {
    auto cmd = RedisVectorStore::build_hset_command(
        "docs:", {"doc_0", "hello", {1.0f, 2.0f}, json{{"src", "test"}}});

    CHECK(cmd[0] == "HSET");
    CHECK(cmd[1] == "docs:doc_0");
    CHECK(cmd[2] == "content");
    CHECK(cmd[3] == "hello");
    CHECK(cmd[4] == "metadata");
    CHECK(json::parse(cmd[5])["src"] == "test");
    CHECK(cmd[6] == "embedding");
    CHECK(cmd[7] == RedisVectorStore::encode_vector({1.0f, 2.0f}));

    // Null metadata becomes an empty object rather than the string "null".
    auto empty = RedisVectorStore::build_hset_command("docs:", {"d", "c", {1.0f}, json{}});
    CHECK(empty[5] == "{}");
}

TEST_CASE("FT.SEARCH asks for KNN with the vector as a parameter") {
    std::string blob = RedisVectorStore::encode_vector({1.0f, 0.0f});
    auto        cmd  = RedisVectorStore::build_search_command("docs", blob, 2);

    CHECK(cmd[0] == "FT.SEARCH");
    CHECK(cmd[1] == "docs");
    CHECK(cmd[2] == "*=>[KNN 2 @embedding $BLOB AS vector_score]");
    CHECK(cmd[3] == "PARAMS");
    CHECK(cmd[4] == "2");
    CHECK(cmd[5] == "BLOB");
    CHECK(cmd[6] == blob);            // binary, never spliced into the query text

    auto arg = [&](const std::string& name) {
        for (std::size_t i = 0; i < cmd.size(); ++i)
            if (cmd[i] == name) return i;
        return cmd.size();
    };
    REQUIRE(arg("SORTBY") < cmd.size());
    CHECK(cmd[arg("SORTBY") + 1] == "vector_score");
    REQUIRE(arg("LIMIT") < cmd.size());
    CHECK(cmd[arg("LIMIT") + 1] == "0");
    CHECK(cmd[arg("LIMIT") + 2] == "2");
    // Vector query syntax only exists from dialect 2 on.
    REQUIRE(arg("DIALECT") < cmd.size());
    CHECK(cmd[arg("DIALECT") + 1] == "2");

    // A nonsensical top_k still produces a valid KNN clause.
    auto zero = RedisVectorStore::build_search_command("docs", blob, 0);
    CHECK(zero[2] == "*=>[KNN 1 @embedding $BLOB AS vector_score]");
}

TEST_CASE("counting is a search that asks for no rows") {
    auto cmd = RedisVectorStore::build_count_command("docs");
    CHECK(cmd == std::vector<std::string>{"FT.SEARCH", "docs", "*",
                                          "LIMIT", "0", "0", "DIALECT", "2"});
}

TEST_CASE("SCAN patterns escape glob metacharacters in the prefix") {
    CHECK(RedisVectorStore::glob_escape("docs:") == "docs:");
    CHECK(RedisVectorStore::glob_escape("a*b?c[d]") == "a\\*b\\?c\\[d\\]");
}

// ═══════════════════════════════════════════════════════════════════════════
// Reply shaping
// ═══════════════════════════════════════════════════════════════════════════

// Builds the reply a live FT.SEARCH produces: total, then key/field-array pairs.
static Reply search_reply(const std::vector<std::pair<std::string,
                                                      std::vector<std::string>>>& docs) {
    Reply out;
    out.type = Reply::Type::array;

    Reply total;
    total.type    = Reply::Type::integer;
    total.integer = static_cast<long long>(docs.size());
    out.elements.push_back(total);

    for (const auto& [key, fields] : docs) {
        Reply k;
        k.type = Reply::Type::bulk;
        k.str  = key;
        out.elements.push_back(k);

        Reply f;
        f.type = Reply::Type::array;
        for (const auto& v : fields) {
            Reply item;
            item.type = Reply::Type::bulk;
            item.str  = v;
            f.elements.push_back(item);
        }
        out.elements.push_back(f);
    }
    return out;
}

TEST_CASE("search results strip the key prefix and invert the distance") {
    auto reply = search_reply({
        {"docs:doc_a", {"content", "the cat sat", "metadata", R"({"topic":"pets"})",
                        "vector_score", "0.05"}},
        {"docs:doc_b", {"content", "quantum field theory", "metadata", "{}",
                        "vector_score", "0.40"}}});

    auto hits = RedisVectorStore::parse_search_reply(reply, "docs:");
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == "doc_a");                 // the id stored, not the Redis key
    CHECK(hits[0].content == "the cat sat");
    CHECK(hits[0].metadata["topic"] == "pets");
    // RediSearch reports distance (lower is closer); tiny_agent returns similarity.
    CHECK(hits[0].score == doctest::Approx(0.95f));
    CHECK(hits[1].score == doctest::Approx(0.60f));
    CHECK(hits[0].score > hits[1].score);
}

TEST_CASE("a key written without the prefix still comes back with an id") {
    auto reply = search_reply({{"loose_key", {"content", "x", "vector_score", "0.0"}}});
    auto hits  = RedisVectorStore::parse_search_reply(reply, "docs:");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "loose_key");
}

TEST_CASE("metadata that is not an object loses the metadata, not the hit") {
    auto reply = search_reply({{"docs:a", {"content", "x", "metadata", "not json",
                                           "vector_score", "0.1"}}});
    auto hits  = RedisVectorStore::parse_search_reply(reply, "docs:");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "x");
    CHECK(hits[0].metadata.is_object());
    CHECK(hits[0].metadata.empty());
}

TEST_CASE("an empty result set parses to no hits") {
    Reply reply;
    reply.type = Reply::Type::array;
    Reply zero;
    zero.type = Reply::Type::integer;
    reply.elements.push_back(zero);
    CHECK(RedisVectorStore::parse_search_reply(reply, "docs:").empty());
}

TEST_CASE("a server error surfaces as an Error, not an empty result") {
    Reply err;
    err.type = Reply::Type::error;
    err.str  = "docs: no such index";
    CHECK_THROWS_AS(RedisVectorStore::parse_search_reply(err, "docs:"), Error);
}

TEST_CASE("distance converts to similarity per metric") {
    // Cosine and inner-product distances are both 1 - similarity.
    CHECK(RedisVectorStore::distance_to_similarity("COSINE", 0.0f) == doctest::Approx(1.0f));
    CHECK(RedisVectorStore::distance_to_similarity("COSINE", 2.0f) == doctest::Approx(-1.0f));
    CHECK(RedisVectorStore::distance_to_similarity("IP", 0.25f) == doctest::Approx(0.75f));

    // L2 is unbounded above, so it maps monotonically into (0, 1] instead.
    CHECK(RedisVectorStore::distance_to_similarity("L2", 0.0f) == doctest::Approx(1.0f));
    CHECK(RedisVectorStore::distance_to_similarity("L2", 1.0f) == doctest::Approx(0.5f));
    CHECK(RedisVectorStore::distance_to_similarity("L2", 3.0f)
        < RedisVectorStore::distance_to_similarity("L2", 1.0f));
}

TEST_CASE("Redis satisfies both store concepts") {
    static_assert(vector_store<RedisVectorStore>);
    static_assert(batch_vector_store<RedisVectorStore>);
}

// ═══════════════════════════════════════════════════════════════════════════
// Live round-trips — skipped unless REDIS_URL is set
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("live Redis round-trip") {
    const char* url = std::getenv("REDIS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    RedisVectorStore store{url, "tiny_agent_test"};
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
    CHECK(hits[0].score > hits[1].score);

    // Re-adding the same id updates rather than duplicating.
    store.add("doc_a", "the cat sat on the mat", {1.0f, 0.0f, 0.0f}, json{{"topic", "pets"}});
    CHECK(store.size() == 3);

    store.clear();
    CHECK(store.size() == 0);
}

TEST_CASE("live Redis ranks the same way FlatVectorStore does") {
    const char* url = std::getenv("REDIS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    const std::vector<Document> corpus = {
        {"doc_a", "the cat sat on the mat", {0.90f, 0.10f, 0.05f}, json{{"topic", "pets"}}},
        {"doc_b", "the dog ran in the park", {0.70f, 0.50f, 0.10f}, json{{"topic", "pets"}}},
        {"doc_c", "quantum field theory",    {0.05f, 0.20f, 0.95f}, json{{"topic", "physics"}}},
        {"doc_d", "a kitten on a rug",       {0.85f, 0.20f, 0.10f}, json{{"topic", "pets"}}}};
    const std::vector<float> query = {0.88f, 0.15f, 0.07f};

    FlatVectorStore flat;
    add_documents(flat, corpus);

    RedisVectorStore store{url, "tiny_agent_rank_test"};
    store.clear();
    add_documents(store, corpus);

    auto flat_hits  = flat.search(query, 4);
    auto redis_hits = store.search(query, 4);
    REQUIRE(redis_hits.size() == flat_hits.size());

    for (std::size_t i = 0; i < flat_hits.size(); ++i) {
        CHECK(redis_hits[i].id == flat_hits[i].id);
        // Cosine distance inverted is cosine similarity, so the scores match
        // too, not only the order. float32 on the wire sets the tolerance.
        CHECK(redis_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001));
    }
    store.clear();
}

TEST_CASE("live Redis behind a Retriever") {
    const char* url = std::getenv("REDIS_URL");
    if (!url) {
        REQUIRE_FALSE(url);
        return;
    }
    RedisVectorStore store{url, "tiny_agent_retriever_test"};
    store.clear();

    Retriever<MockEmbed, RedisVectorStore> r{std::move(store), MockEmbed{}, 2};
    r.add_documents({"the cat sat", "quantum mechanics"},
                    {json{{"topic", "pets"}}, json{{"topic", "physics"}}});

    auto hits = r.query("the cat sat", 1);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].content == "the cat sat");
    r.store().clear();
}
