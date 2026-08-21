#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  vectorstore/weaviate.hpp  —  Weaviate over its HTTP API
//
//  Needs a running Weaviate and nothing else: no client library, no gRPC, just
//  the httplib already in the build.
//
//    auto store = WeaviateVectorStore{"http://localhost:8080", "my_docs",
//                                     WeaviateConfig{.api_key = "…"}};
//    store.add("doc_1", "content", embedding, metadata);
//    auto hits = store.search(query_vec, 5);
//
//  Four things about Weaviate shape the adapter:
//
//  - Collection names are GraphQL type names. Weaviate capitalises the first
//    letter for you and then keys every later path off the capitalised form, so
//    the adapter does the capitalising up front and rejects a name GraphQL
//    cannot express.
//  - Vector search has no REST endpoint. /v1/objects lists and filters; the
//    only near-vector path over HTTP is POST /v1/graphql. That is the one this
//    adapter uses, so search() builds a GraphQL query string rather than JSON.
//  - Writes go to POST /v1/batch/objects, which answers 200 even when
//    individual objects fail. The per-object result carries the status, so a
//    caller that only checked the HTTP code would lose writes silently.
//  - Object ids must be UUIDs, same as Qdrant, so the adapter hashes the
//    caller's id and keeps the original in a property.
//
//  Metadata is stored as one JSON text property. Nothing is flattened or
//  dropped, and it comes back as the object you put in; the trade is that it is
//  opaque to Weaviate's `where` filters, which this interface does not expose.
// ═══════════════════════════════════════════════════════════════════════════════

#include "base.hpp"
#include "../core/log.hpp"
#include <httplib.h>
#include <string>

namespace tiny_agent {

struct WeaviateConfig {
    std::string api_key;                 // sent as "Authorization: Bearer …"
    std::string distance = "cosine";     // cosine | dot | l2-squared | hamming | manhattan
    int         timeout_seconds = 30;
    Log         log;
};

namespace detail {

// A Weaviate collection is a GraphQL type, so its name has to be a GraphQL type
// name: an initial letter, then letters, digits and underscores. The server
// capitalises the first character silently and stores the result, which means a
// caller that passes "my_docs" and then addresses "my_docs" gets a 404 from
// every path except the create. Capitalise here so the name the adapter holds
// is the name the server holds.
inline std::string weaviate_class_name(const std::string& name) {
    if (name.empty())
        throw Error("WeaviateVectorStore: collection name must not be empty");

    std::string out = name;
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    if (!(out[0] >= 'A' && out[0] <= 'Z'))
        throw Error("WeaviateVectorStore: collection name must start with a letter, got '"
            + name + "'");

    for (char c : out) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            throw Error("WeaviateVectorStore: collection name may hold only letters, "
                "digits and underscores, got '" + name + "'");
    }
    return out;
}

} // namespace detail

class WeaviateVectorStore {
    std::string     base_url_;
    std::string     class_name_;         // capitalised, matches what the server stores
    WeaviateConfig  config_;
    // mutable so search() and size() stay const for callers: issuing a request
    // does not change what the store logically holds.
    mutable httplib::Client client_;
    bool            class_ensured_ = false;

    [[nodiscard]] std::string schema_path() const {
        return "/v1/schema/" + class_name_;
    }

    static json parse_or_throw(const std::string& body, const char* what) {
        try {
            return json::parse(body);
        } catch (const std::exception& e) {
            throw Error(std::string("WeaviateVectorStore::") + what
                + ": server returned invalid JSON: " + e.what());
        }
    }

    void check(const httplib::Result& res, const char* what) const {
        if (!res)
            throw Error(std::string("WeaviateVectorStore::") + what + " failed: "
                + httplib::to_string(res.error()));
        if (res->status < 200 || res->status >= 300)
            throw Error(std::string("WeaviateVectorStore::") + what + " rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
    }

    // GraphQL is a POST that answers 200 whatever happened; the failure lives in
    // the body. Everything that goes through /v1/graphql comes back through here.
    [[nodiscard]] json graphql(const std::string& query, const char* what) const {
        auto res = client_.Post("/v1/graphql",
                                json{{"query", query}}.dump(), "application/json");
        check(res, what);
        return parse_or_throw(res->body, what);
    }

    void ensure_class() {
        if (class_ensured_) return;

        auto res = client_.Get(schema_path());
        if (res && res->status == 200) {
            class_ensured_ = true;
            return;
        }

        config_.log.info("weaviate", "creating collection '" + class_name_
            + "' (distance=" + config_.distance + ", vectorizer=none)");

        auto create = client_.Post("/v1/schema", build_schema_body(class_name_,
            config_.distance).dump(), "application/json");
        if (!create)
            throw Error("WeaviateVectorStore: cannot reach " + base_url_ + ": "
                + httplib::to_string(create.error()));
        // A concurrent writer may have created it between the GET and the POST;
        // that is a success, not a failure. Weaviate answers 422 for that.
        if ((create->status < 200 || create->status >= 300)
            && create->body.find("already exists") == std::string::npos)
            throw Error("WeaviateVectorStore: failed to create collection '"
                + class_name_ + "' (status " + std::to_string(create->status)
                + "): " + create->body.substr(0, 512));
        class_ensured_ = true;
    }

public:
    WeaviateVectorStore(std::string base_url, const std::string& collection,
                        WeaviateConfig cfg = {})
        : base_url_(std::move(base_url))
        , class_name_(detail::weaviate_class_name(collection))
        , config_(std::move(cfg))
        , client_(base_url_)
    {
        client_.set_read_timeout(config_.timeout_seconds);
        client_.set_write_timeout(config_.timeout_seconds);
        if (!config_.api_key.empty())
            client_.set_default_headers({{"Authorization", "Bearer " + config_.api_key}});
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    // ── Wire format, as pure functions a test can check without a server ─────

    // Vectors come from the caller, so the collection declares vectorizer
    // "none". Without that, Weaviate falls back to whatever DEFAULT_VECTORIZER_
    // MODULE the server was started with and tries to embed the text itself.
    //
    // No dimension here: Weaviate sizes the index from the first object, unlike
    // Qdrant, which wants the width at creation.
    static json build_schema_body(const std::string& class_name,
                                  const std::string& distance) {
        return {
            {"class",      class_name},
            {"vectorizer", "none"},
            {"vectorIndexConfig", {{"distance", distance}}},
            {"properties", json::array({
                json{{"name", "content"},       {"dataType", json::array({"text"})}},
                json{{"name", "tiny_agent_id"}, {"dataType", json::array({"text"})}},
                json{{"name", "metadata"},      {"dataType", json::array({"text"})}}})}};
    }

    // Documents in, batch body out. Each object names its own collection, which
    // is how one batch can span several.
    static json build_batch_body(const std::string& class_name,
                                 const std::vector<Document>& docs) {
        json objects = json::array();
        for (const auto& d : docs) {
            json o;
            o["class"]  = class_name;
            o["id"]     = detail::uuid_from_string(d.id);
            o["vector"] = d.embedding;
            o["properties"] = {
                {"content",       d.content},
                {"tiny_agent_id", d.id},
                {"metadata",      d.metadata.is_object() ? d.metadata.dump()
                                                         : std::string("{}")}};
            objects.push_back(std::move(o));
        }
        return {{"objects", std::move(objects)}};
    }

    // The batch endpoint answers 200 and reports per-object failures in the
    // body. Checking only the HTTP status loses writes without saying so.
    static void check_batch_response(const json& parsed) {
        if (!parsed.is_array())
            throw Error("WeaviateVectorStore::add: unexpected batch response shape: "
                + parsed.dump().substr(0, 512));

        std::string failures;
        for (const auto& o : parsed) {
            auto result = o.value("result", json::object());
            if (result.value("status", std::string("SUCCESS")) == "SUCCESS") continue;
            if (!failures.empty()) failures += "; ";
            failures += o.value("id", std::string("?")) + ": "
                      + result.value("errors", json::object()).dump();
        }
        if (!failures.empty())
            throw Error("WeaviateVectorStore::add: " + failures.substr(0, 512));
    }

    // Near-vector search is GraphQL only, so this builds query text rather than
    // a JSON body. Nothing the caller supplies is interpolated as a string: the
    // collection name was validated in the constructor and the rest is numbers.
    static std::string build_search_query(const std::string& class_name,
                                          const std::vector<float>& query,
                                          int top_k) {
        return "{ Get { " + class_name
             + "(nearVector: {vector: " + json(query).dump()
             + "}, limit: " + std::to_string(top_k > 0 ? top_k : 1)
             + ") { content tiny_agent_id metadata"
               " _additional { id distance } } } }";
    }

    // Weaviate reports a *distance*, lower is better; tiny_agent returns a
    // similarity. With the default cosine metric, 1 - distance is the cosine
    // similarity FlatVectorStore computes, so the two rank and score alike.
    static std::vector<SearchResult> parse_query_response(const json& parsed,
                                                          const std::string& class_name) {
        if (parsed.contains("errors") && !parsed["errors"].empty())
            throw Error("WeaviateVectorStore::search rejected: "
                + parsed["errors"].dump().substr(0, 512));
        if (!parsed.contains("data") || !parsed["data"].contains("Get"))
            throw Error("WeaviateVectorStore::search: unexpected response shape: "
                + parsed.dump().substr(0, 512));

        const auto& get = parsed["data"]["Get"];
        if (!get.contains(class_name) || !get[class_name].is_array())
            throw Error("WeaviateVectorStore::search: response carried no '"
                + class_name + "' hits: " + parsed.dump().substr(0, 512));

        std::vector<SearchResult> out;
        out.reserve(get[class_name].size());
        for (const auto& hit : get[class_name]) {
            auto extra = hit.value("_additional", json::object());
            // Prefer the id we stored; fall back to the UUID Weaviate echoes.
            std::string id = hit.value("tiny_agent_id", std::string{});
            if (id.empty()) id = extra.value("id", std::string{});

            float distance = extra.contains("distance") && extra["distance"].is_number()
                ? extra["distance"].get<float>() : 0.0f;

            out.push_back({std::move(id),
                           hit.value("content", std::string{}),
                           1.0f - distance,
                           parse_metadata(hit.value("metadata", std::string{}))});
        }
        return out;
    }

    // Metadata round-trips through a text property, so it comes back as the
    // object that went in. A value written by something other than this adapter
    // may not be JSON at all; that reads as no metadata rather than throwing.
    static json parse_metadata(const std::string& raw) {
        if (raw.empty()) return json::object();
        auto parsed = json::parse(raw, nullptr, false);
        return parsed.is_object() ? parsed : json::object();
    }

    // ── The four-method interface ───────────────────────────────────────────

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        add_batch({{id, content, embedding, metadata}});
    }

    void add_batch(const std::vector<Document>& docs) {
        if (docs.empty()) return;
        ensure_class();

        auto body = build_batch_body(class_name_, docs);
        // consistency_level=ALL so a search issued right after this call sees
        // the objects.
        auto res = client_.Post("/v1/batch/objects?consistency_level=ALL",
                                body.dump(), "application/json");
        check(res, "add");
        check_batch_response(parse_or_throw(res->body, "add"));
        config_.log.debug("weaviate", "wrote " + std::to_string(docs.size()) + " object(s)");
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) const {
        return parse_query_response(
            graphql(build_search_query(class_name_, query, top_k), "search"),
            class_name_);
    }

    // Counted server-side rather than tracked locally, so a collection another
    // process also writes to reports the truth.
    [[nodiscard]] std::size_t size() const {
        auto res = client_.Post("/v1/graphql",
            json{{"query", "{ Aggregate { " + class_name_ + " { meta { count } } } }"}}.dump(),
            "application/json");
        if (!res || res->status != 200) return 0;

        auto parsed = json::parse(res->body, nullptr, false);
        // A collection that does not exist is not a GraphQL field, so this comes
        // back as an error rather than a zero. Empty is the honest answer.
        if (!parsed.is_object() || !parsed.contains("data")) return 0;
        const auto& agg = parsed["data"].value("Aggregate", json::object());
        if (!agg.contains(class_name_) || !agg[class_name_].is_array()
            || agg[class_name_].empty()) return 0;
        return agg[class_name_][0].value("meta", json::object())
                                  .value("count", std::size_t{0});
    }

    // Deletes the collection rather than the objects in it: Weaviate's object
    // delete takes one id at a time, and the batch delete needs a `where` clause
    // that matches everything. Dropping the collection is one call and the next
    // write recreates it. A missing collection deletes cleanly, which is the
    // state clear() wanted.
    void clear() {
        auto res = client_.Delete(schema_path());
        if (!res)
            throw Error("WeaviateVectorStore::clear failed: "
                + httplib::to_string(res.error()));
        if ((res->status < 200 || res->status >= 300) && res->status != 404)
            throw Error("WeaviateVectorStore::clear rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
        class_ensured_ = false;
    }

    // The capitalised name, which is what the server stores.
    [[nodiscard]] const std::string& collection() const { return class_name_; }
};

static_assert(vector_store<WeaviateVectorStore>);
static_assert(batch_vector_store<WeaviateVectorStore>);

} // namespace tiny_agent
