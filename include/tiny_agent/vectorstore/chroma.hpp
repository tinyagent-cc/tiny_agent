#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  vectorstore/chroma.hpp  —  Chroma over its v2 REST API
//
//  Needs a running `chroma run` and nothing else.
//
//    auto store = ChromaVectorStore{"http://localhost:8000", "my_docs"};
//    store.add("doc_1", "content", embedding, metadata);
//    auto hits = store.search(query_vec, 5);
//
//  Two things about Chroma's v2 API shape the adapter:
//
//  - Every record path is keyed by the collection's UUID, not its name, so the
//    first write resolves the name to an id via get_or_create and caches it.
//  - Query returns a *distance*, lower is better, and one array per query
//    embedding. search() unwraps the single query and converts cosine distance
//    to a similarity so it reads the same way as every other store here.
//
//  Metadata values must be scalars (string, number, bool). Nested objects are
//  flattened to their JSON text rather than being silently dropped.
// ═══════════════════════════════════════════════════════════════════════════════

#include "base.hpp"
#include "../core/log.hpp"
#include <httplib.h>

namespace tiny_agent {

struct ChromaConfig {
    std::string tenant   = "default_tenant";
    std::string database = "default_database";
    std::string api_token;                  // sent as "X-Chroma-Token"
    std::string space = "cosine";           // cosine | l2 | ip, fixed at creation
    int         timeout_seconds = 30;
    Log         log;
};

class ChromaVectorStore {
    std::string     base_url_;
    std::string     collection_name_;
    ChromaConfig    config_;
    httplib::Client client_;
    std::string     collection_id_;         // resolved UUID, cached after the first call

    [[nodiscard]] std::string collections_path() const {
        return "/api/v2/tenants/" + config_.tenant
             + "/databases/" + config_.database + "/collections";
    }

    [[nodiscard]] std::string record_path(const std::string& op) const {
        return collections_path() + "/" + collection_id_ + "/" + op;
    }

    static json parse_or_throw(const std::string& body, const char* what) {
        try {
            return json::parse(body);
        } catch (const std::exception& e) {
            throw Error(std::string("ChromaVectorStore::") + what
                + ": server returned invalid JSON: " + e.what());
        }
    }

    void check(const httplib::Result& res, const char* what) const {
        if (!res)
            throw Error(std::string("ChromaVectorStore::") + what + " failed: "
                + httplib::to_string(res.error()));
        if (res->status < 200 || res->status >= 300)
            throw Error(std::string("ChromaVectorStore::") + what + " rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
    }

    const std::string& ensure_collection() {
        if (!collection_id_.empty()) return collection_id_;

        json body;
        body["name"]          = collection_name_;
        body["get_or_create"] = true;
        if (!config_.space.empty())
            body["configuration"] = {{"hnsw", {{"space", config_.space}}}};

        auto res = client_.Post(collections_path(), body.dump(), "application/json");
        check(res, "ensure_collection");

        auto parsed = parse_or_throw(res->body, "ensure_collection");
        if (!parsed.contains("id") || !parsed["id"].is_string())
            throw Error("ChromaVectorStore: collection response carried no id: "
                + res->body.substr(0, 512));
        collection_id_ = parsed["id"].get<std::string>();
        config_.log.debug("chroma", "collection '" + collection_name_
            + "' resolved to " + collection_id_);
        return collection_id_;
    }

public:
    ChromaVectorStore(std::string base_url, std::string collection,
                      ChromaConfig cfg = {})
        : base_url_(std::move(base_url))
        , collection_name_(std::move(collection))
        , config_(std::move(cfg))
        , client_(base_url_)
    {
        if (collection_name_.empty())
            throw Error("ChromaVectorStore: collection name must not be empty");
        client_.set_read_timeout(config_.timeout_seconds);
        client_.set_write_timeout(config_.timeout_seconds);
        if (!config_.api_token.empty())
            client_.set_default_headers({{"X-Chroma-Token", config_.api_token}});
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    // Chroma metadata values must be scalars. Anything structured is stored as
    // its JSON text so nothing is lost, just flattened.
    static json flatten_metadata(const json& meta) {
        json out = json::object();
        if (!meta.is_object()) return out;
        for (const auto& [k, v] : meta.items()) {
            if (v.is_string() || v.is_number() || v.is_boolean()) out[k] = v;
            else if (v.is_null())                                 continue;
            else                                                  out[k] = v.dump();
        }
        return out;
    }

    // Pure: documents in, upsert body out.
    static json build_upsert_body(const std::vector<Document>& docs) {
        json ids = json::array(), embeddings = json::array();
        json documents = json::array(), metadatas = json::array();
        for (const auto& d : docs) {
            ids.push_back(d.id);
            embeddings.push_back(d.embedding);
            documents.push_back(d.content);
            metadatas.push_back(flatten_metadata(d.metadata));
        }
        return {{"ids", std::move(ids)}, {"embeddings", std::move(embeddings)},
                {"documents", std::move(documents)}, {"metadatas", std::move(metadatas)}};
    }

    // Chroma answers with one array per query embedding; this unwraps the single
    // query and turns cosine distance into a similarity (higher is closer),
    // matching what every other store in tiny_agent returns.
    static std::vector<SearchResult> parse_query_response(const json& parsed) {
        auto first = [&](const char* key) -> json {
            if (!parsed.contains(key) || !parsed[key].is_array()
                || parsed[key].empty() || parsed[key][0].is_null())
                return json::array();
            return parsed[key][0];
        };

        auto ids       = first("ids");
        auto documents = first("documents");
        auto metadatas = first("metadatas");
        auto distances = first("distances");

        std::vector<SearchResult> out;
        out.reserve(ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            float distance = i < distances.size() && distances[i].is_number()
                ? distances[i].get<float>() : 0.0f;
            out.push_back({
                ids[i].is_string() ? ids[i].get<std::string>() : ids[i].dump(),
                i < documents.size() && documents[i].is_string()
                    ? documents[i].get<std::string>() : std::string{},
                1.0f - distance,
                i < metadatas.size() && metadatas[i].is_object()
                    ? metadatas[i] : json::object()
            });
        }
        return out;
    }

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        add_batch({{id, content, embedding, metadata}});
    }

    void add_batch(const std::vector<Document>& docs) {
        if (docs.empty()) return;
        ensure_collection();
        auto body = build_upsert_body(docs);
        auto res = client_.Post(record_path("upsert"), body.dump(), "application/json");
        check(res, "add");
        config_.log.debug("chroma", "upserted " + std::to_string(docs.size()) + " record(s)");
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) {
        ensure_collection();
        json body;
        body["query_embeddings"] = json::array({query});
        body["n_results"]        = top_k;
        body["include"]          = json::array({"documents", "metadatas", "distances"});

        auto res = client_.Post(record_path("query"), body.dump(), "application/json");
        check(res, "search");
        return parse_query_response(parse_or_throw(res->body, "search"));
    }

    [[nodiscard]] std::size_t size() {
        ensure_collection();
        auto res = client_.Get(record_path("count"));
        if (!res || res->status != 200) return 0;
        auto parsed = parse_or_throw(res->body, "size");
        return parsed.is_number_unsigned() ? parsed.get<std::size_t>() : 0;
    }

    // Deletes the collection outright rather than filtering records: Chroma's
    // record-delete endpoint removes everything when given neither ids nor a
    // where clause, which is a sharp edge worth not standing on.
    //
    // Note the path takes the collection *name*, unlike add/query/count which
    // take the UUID. Deleting by UUID answers 404, so a clear() that used the
    // id here would look like it worked and change nothing.
    void clear() {
        auto res = client_.Delete(collections_path() + "/" + collection_name_);
        if (!res)
            throw Error("ChromaVectorStore::clear failed: "
                + httplib::to_string(res.error()));
        if (res->status != 200 && res->status != 404)
            throw Error("ChromaVectorStore::clear rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
        collection_id_.clear();
    }

    [[nodiscard]] const std::string& collection() const { return collection_name_; }
    [[nodiscard]] const std::string& collection_id() const { return collection_id_; }
};

static_assert(vector_store<ChromaVectorStore>);
static_assert(batch_vector_store<ChromaVectorStore>);

} // namespace tiny_agent
