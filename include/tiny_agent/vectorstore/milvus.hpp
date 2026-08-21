#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  vectorstore/milvus.hpp  —  Milvus over its v2 RESTful API
//
//  Needs a running Milvus and nothing else: no SDK, no gRPC, just the httplib
//  already in the build.
//
//    auto store = MilvusVectorStore{"http://localhost:19530", "my_docs",
//                                   MilvusConfig{.token = "…"}};
//    store.add("doc_1", "content", embedding, metadata);
//    auto hits = store.search(query_vec, 5);
//
//  Milvus serves REST and gRPC on the same port, 19530, under /v2/vectordb.
//  Port 9091 answers /healthz and nothing else.
//
//  Five things about Milvus shape the adapter:
//
//  - A rejected request comes back HTTP 200 with a non-zero "code" in the body.
//    Reading the status alone turns a dimension mismatch into a silent success,
//    so every call checks the body.
//  - Writing goes to /entities/upsert, not /entities/insert. Insert appends
//    without looking at the primary key, so re-adding a document duplicates it
//    and the corpus grows every time you re-index.
//  - A collection answers no read until it is loaded into memory, and the load
//    that follows creation is asynchronous. A search issued straight after the
//    first write comes back "collection not loaded", so reads wait for the load
//    to finish first.
//  - /collections/get_stats counts only sealed segments, so it reports 0 for a
//    collection whose writes are still in memory. size() runs a count(*) query
//    instead, which sees everything.
//  - With the default COSINE metric the "distance" in a hit is already a
//    similarity, higher meaning closer, so nothing is inverted. Chroma and
//    Weaviate both report a real distance and their adapters convert.
//
//  Ids are free-form strings on the wire: the primary key is a VarChar, so the
//  id you store is the id Milvus holds and the id search() gives back. Qdrant
//  and Weaviate accept only UUIDs and their adapters hash around it; this one
//  has nothing to hash.
// ═══════════════════════════════════════════════════════════════════════════════

#include "base.hpp"
#include "../core/log.hpp"
#include <httplib.h>
#include <chrono>
#include <thread>
#include <string>

namespace tiny_agent {

struct MilvusConfig {
    std::string token;                    // sent as "Authorization: Bearer …"
    std::string database;                 // empty leaves the server's default
    std::string metric_type = "COSINE";   // COSINE | IP | L2
    std::string index_type  = "AUTOINDEX";
    int         max_id_length      = 512;
    int         max_content_length = 65535;
    int         timeout_seconds      = 30;
    int         load_timeout_seconds = 60;
    Log         log;
};

class MilvusVectorStore {
    std::string   base_url_;
    std::string   collection_;
    MilvusConfig  config_;
    // mutable so search() and size() stay const for callers: issuing a request,
    // or waiting for the server to finish loading, does not change what the
    // store logically holds.
    mutable httplib::Client client_;
    mutable bool  collection_ensured_ = false;
    mutable bool  loaded_             = false;

    static std::string path(const char* endpoint) {
        return std::string("/v2/vectordb/") + endpoint;
    }

    // Every request names the collection, and optionally the database.
    [[nodiscard]] json base_body() const {
        json b{{"collectionName", collection_}};
        if (!config_.database.empty()) b["dbName"] = config_.database;
        return b;
    }

    static json parse_or_throw(const std::string& body, const char* what) {
        try {
            return json::parse(body);
        } catch (const std::exception& e) {
            throw Error(std::string("MilvusVectorStore::") + what
                + ": server returned invalid JSON: " + e.what());
        }
    }

    // Transport and HTTP failures throw here; a business failure rides in the
    // body and is left for the caller to read or ignore.
    [[nodiscard]] json request(const char* endpoint, const json& body,
                               const char* what) const {
        auto res = client_.Post(path(endpoint), body.dump(), "application/json");
        if (!res)
            throw Error(std::string("MilvusVectorStore::") + what + " failed: "
                + httplib::to_string(res.error()));
        if (res->status < 200 || res->status >= 300)
            throw Error(std::string("MilvusVectorStore::") + what + " rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
        return parse_or_throw(res->body, what);
    }

    // Not nodiscard: create, load, upsert and drop all care only that the call
    // was accepted, and the check for that happens in here.
    json call(const char* endpoint, const json& body, const char* what) const {
        auto parsed = request(endpoint, body, what);
        check_code(parsed, what);
        return parsed;
    }

    [[nodiscard]] bool collection_exists() const {
        auto parsed = call("collections/has", base_body(), "has");
        return parsed.value("data", json::object()).value("has", false);
    }

    // A collection answers no read until it is in memory. Creation kicks off an
    // asynchronous load, and a collection somebody else released has to be asked
    // again, so both cases land here. False means there is no such collection,
    // which size() reads as empty rather than as an error.
    bool ensure_loaded() const {
        if (loaded_) return true;

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(config_.load_timeout_seconds);
        bool load_requested = false;
        std::string state;

        for (;;) {
            auto parsed = request("collections/get_load_state", base_body(), "load");
            if (parsed.value("code", 0) != 0) return false;

            state = parsed.value("data", json::object())
                          .value("loadState", std::string{});
            if (state == "LoadStateLoaded") {
                loaded_ = true;
                return true;
            }
            // "NotLoad" stays that way until something asks. "Loading" is already
            // on its way and only needs waiting out.
            if (state == "LoadStateNotLoad" && !load_requested) {
                config_.log.debug("milvus", "loading collection '" + collection_ + "'");
                call("collections/load", base_body(), "load");
                load_requested = true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error("MilvusVectorStore: collection '" + collection_
                    + "' was still " + state + " after "
                    + std::to_string(config_.load_timeout_seconds) + "s");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void ensure_collection(int dims) const {
        if (collection_ensured_) return;

        if (!collection_exists()) {
            config_.log.info("milvus", "creating collection '" + collection_
                + "' (dims=" + std::to_string(dims)
                + " metric=" + config_.metric_type + ")");
            call("collections/create",
                 with_database(build_create_body(collection_, dims, config_.metric_type,
                     config_.index_type, config_.max_id_length,
                     config_.max_content_length)),
                 "create");
        }
        // Creating with indexParams starts the load; joining it here means the
        // first search after the first write does not race it.
        ensure_loaded();
        collection_ensured_ = true;
    }

    [[nodiscard]] json with_database(json body) const {
        if (!config_.database.empty()) body["dbName"] = config_.database;
        return body;
    }

public:
    MilvusVectorStore(std::string base_url, std::string collection,
                      MilvusConfig cfg = {})
        : base_url_(std::move(base_url))
        , collection_(std::move(collection))
        , config_(std::move(cfg))
        , client_(base_url_)
    {
        if (collection_.empty())
            throw Error("MilvusVectorStore: collection name must not be empty");
        client_.set_read_timeout(config_.timeout_seconds);
        client_.set_write_timeout(config_.timeout_seconds);
        if (!config_.token.empty())
            client_.set_default_headers({{"Authorization", "Bearer " + config_.token}});
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    // ── Wire format, as pure functions a test can check without a server ─────

    // The schema is spelled out rather than left to Milvus's quick setup, which
    // gives an Int64 primary key and the Bounded consistency level. A VarChar key
    // is what lets the caller keep its own ids, and Strong is what makes a search
    // issued right after a write see it.
    static json build_create_body(const std::string& collection, int dims,
                                  const std::string& metric_type,
                                  const std::string& index_type,
                                  int max_id_length, int max_content_length) {
        json fields = json::array({
            json{{"fieldName", "id"}, {"dataType", "VarChar"}, {"isPrimary", true},
                 {"elementTypeParams", {{"max_length", std::to_string(max_id_length)}}}},
            json{{"fieldName", "vector"}, {"dataType", "FloatVector"},
                 {"elementTypeParams", {{"dim", std::to_string(dims)}}}},
            json{{"fieldName", "content"}, {"dataType", "VarChar"},
                 {"elementTypeParams", {{"max_length", std::to_string(max_content_length)}}}},
            json{{"fieldName", "metadata"}, {"dataType", "JSON"}}});

        return {
            {"collectionName", collection},
            {"schema", {{"autoId", false},
                        {"enabledDynamicField", false},
                        {"fields", std::move(fields)}}},
            // Without indexParams the collection is created unindexed and never
            // loads, and every search fails on a collection that looks fine.
            {"indexParams", json::array({
                json{{"fieldName", "vector"}, {"indexName", "vector_index"},
                     {"metricType", metric_type}, {"indexType", index_type}}})},
            {"params", {{"consistencyLevel", "Strong"}}}};
    }

    // Documents in, upsert body out. Upsert rather than insert: insert ignores
    // the primary key and appends, so re-indexing a corpus doubles it.
    static json build_upsert_body(const std::string& collection,
                                  const std::vector<Document>& docs) {
        json data = json::array();
        for (const auto& d : docs) {
            data.push_back({{"id", d.id},
                            {"vector", d.embedding},
                            {"content", d.content},
                            // A JSON field takes the object itself, so nothing is
                            // flattened the way Chroma needs.
                            {"metadata", d.metadata.is_object() ? d.metadata
                                                                : json::object()}});
        }
        return {{"collectionName", collection}, {"data", std::move(data)}};
    }

    // "data" is an array of query vectors; this interface sends one.
    static json build_search_body(const std::string& collection,
                                  const std::vector<float>& query, int top_k) {
        return {{"collectionName", collection},
                {"data", json::array({query})},
                {"annsField", "vector"},
                {"limit", top_k > 0 ? top_k : 1},
                {"outputFields", json::array({"id", "content", "metadata"})}};
    }

    // An empty filter with count(*) is how Milvus counts rows over REST.
    static json build_count_body(const std::string& collection) {
        return {{"collectionName", collection},
                {"filter", ""},
                {"outputFields", json::array({"count(*)"})}};
    }

    // Milvus calls the number in a hit a "distance" whatever the metric is. For
    // COSINE and IP it is already a similarity and passes through untouched; L2
    // is a real distance, and negating it keeps the interface's "higher is
    // closer" ordering exact.
    static float to_similarity(const std::string& metric_type, float distance) {
        return metric_type == "L2" ? -distance : distance;
    }

    // Milvus answers 200 for a request it refused and puts the failure in the
    // body. Checking only the HTTP status loses writes without saying so.
    static void check_code(const json& parsed, const char* what) {
        int code = parsed.value("code", 0);
        if (code == 0) return;
        throw Error(std::string("MilvusVectorStore::") + what + " rejected (code "
            + std::to_string(code) + "): "
            + parsed.value("message", std::string{}).substr(0, 512));
    }

    // A JSON field comes back over REST as its serialised text rather than as an
    // object, so it is parsed back here. A value written by something other than
    // this adapter may not be an object at all; that reads as no metadata rather
    // than throwing.
    static json parse_metadata(const json& raw) {
        if (raw.is_object()) return raw;
        if (!raw.is_string()) return json::object();
        auto parsed = json::parse(raw.get<std::string>(), nullptr, false);
        return parsed.is_object() ? parsed : json::object();
    }

    static std::vector<SearchResult> parse_search_response(const json& parsed,
                                                           const std::string& metric_type) {
        check_code(parsed, "search");
        if (!parsed.contains("data") || !parsed["data"].is_array())
            throw Error("MilvusVectorStore::search: unexpected response shape: "
                + parsed.dump().substr(0, 512));

        std::vector<SearchResult> out;
        out.reserve(parsed["data"].size());
        for (const auto& hit : parsed["data"]) {
            float distance = hit.contains("distance") && hit["distance"].is_number()
                ? hit["distance"].get<float>() : 0.0f;
            out.push_back({hit.value("id", std::string{}),
                           hit.value("content", std::string{}),
                           to_similarity(metric_type, distance),
                           parse_metadata(hit.value("metadata", json()))});
        }
        return out;
    }

    // ── The four-method interface ───────────────────────────────────────────

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        add_batch({{id, content, embedding, metadata}});
    }

    void add_batch(const std::vector<Document>& docs) {
        if (docs.empty()) return;
        ensure_collection(static_cast<int>(docs.front().embedding.size()));

        call("entities/upsert",
             with_database(build_upsert_body(collection_, docs)), "add");
        config_.log.debug("milvus", "upserted " + std::to_string(docs.size()) + " row(s)");
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) const {
        ensure_loaded();
        return parse_search_response(
            request("entities/search",
                    with_database(build_search_body(collection_, query, top_k)),
                    "search"),
            config_.metric_type);
    }

    // Counted server-side rather than tracked locally, so a collection another
    // process also writes to reports the truth. /collections/get_stats would be
    // the obvious call and it is the wrong one: it counts sealed segments only,
    // so a collection whose rows are still in memory reports zero.
    [[nodiscard]] std::size_t size() const {
        if (!ensure_loaded()) return 0;   // no such collection counts as empty

        auto parsed = request("entities/query",
                              with_database(build_count_body(collection_)), "size");
        if (parsed.value("code", 0) != 0) return 0;
        const auto& rows = parsed["data"];
        if (!rows.is_array() || rows.empty()) return 0;
        return rows[0].value("count(*)", std::size_t{0});
    }

    // Drops the collection rather than deleting rows: Milvus's delete needs a
    // filter expression, and one that matches every row is more work than
    // starting over. The next write recreates it. Dropping something that is not
    // there succeeds, which is the state clear() wanted.
    void clear() {
        call("collections/drop", base_body(), "clear");
        collection_ensured_ = false;
        loaded_             = false;
    }

    [[nodiscard]] const std::string& collection() const { return collection_; }
};

static_assert(vector_store<MilvusVectorStore>);
static_assert(batch_vector_store<MilvusVectorStore>);

} // namespace tiny_agent
