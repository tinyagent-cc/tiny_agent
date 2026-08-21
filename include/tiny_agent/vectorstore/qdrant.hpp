#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  vectorstore/qdrant.hpp  —  Qdrant over its REST API
//
//  Needs a running Qdrant and nothing else: no client library, just the httplib
//  already in the build.
//
//    auto store = QdrantVectorStore{"http://localhost:6333", "my_docs",
//                                   QdrantConfig{.api_key = "…"}};
//    store.add("doc_1", "content", embedding, metadata);
//    auto hits = store.search(query_vec, 5);
//
//  Point ids: Qdrant accepts only an unsigned integer or a UUID, and rejects
//  anything else with 400 ("value doc_0 is not a valid point ID"). Since every
//  other store here takes a free-form string id, this adapter hashes the id into
//  a UUID for the wire and keeps the original in the payload, so search() gives
//  back the id you stored.
// ═══════════════════════════════════════════════════════════════════════════════

#include "base.hpp"
#include "../core/log.hpp"
#include <httplib.h>
#include <cstdint>

namespace tiny_agent {

struct QdrantConfig {
    std::string api_key;
    std::string distance = "Cosine";   // Cosine | Euclid | Dot | Manhattan
    int         timeout_seconds = 30;
    Log         log;
};

// detail::uuid_from_string lives in base.hpp; the Weaviate adapter needs the
// same string-id-to-UUID mapping.

class QdrantVectorStore {
    std::string      base_url_;
    std::string      collection_;
    QdrantConfig     config_;
    // mutable so search() and size() stay const for callers: issuing a request
    // does not change what the store logically holds.
    mutable httplib::Client client_;
    bool             collection_ensured_ = false;
    int              dimensions_         = 0;

    [[nodiscard]] std::string collection_path() const {
        return "/collections/" + collection_;
    }

    static json parse_or_throw(const std::string& body, const char* what) {
        try {
            return json::parse(body);
        } catch (const std::exception& e) {
            throw Error(std::string("QdrantVectorStore::") + what
                + ": server returned invalid JSON: " + e.what());
        }
    }

    void ensure_collection(int dims) {
        if (collection_ensured_) return;
        dimensions_ = dims;

        auto res = client_.Get(collection_path());
        if (res && res->status == 200) {
            collection_ensured_ = true;
            return;
        }

        config_.log.info("qdrant", "creating collection '" + collection_
            + "' (dims=" + std::to_string(dims) + " distance=" + config_.distance + ")");

        json body;
        body["vectors"] = {{"size", dims}, {"distance", config_.distance}};
        auto create = client_.Put(collection_path(), body.dump(), "application/json");
        if (!create)
            throw Error("QdrantVectorStore: cannot reach " + base_url_ + ": "
                + httplib::to_string(create.error()));
        // A concurrent writer may have created it between the GET and the PUT;
        // that is a success, not a failure.
        if (create->status != 200 && create->status != 409
            && create->body.find("already exists") == std::string::npos)
            throw Error("QdrantVectorStore: failed to create collection '"
                + collection_ + "' (status " + std::to_string(create->status)
                + "): " + create->body.substr(0, 512));
        collection_ensured_ = true;
    }

public:
    QdrantVectorStore(std::string base_url, std::string collection,
                      QdrantConfig cfg = {})
        : base_url_(std::move(base_url))
        , collection_(std::move(collection))
        , config_(std::move(cfg))
        , client_(base_url_)
    {
        if (collection_.empty())
            throw Error("QdrantVectorStore: collection name must not be empty");
        client_.set_read_timeout(config_.timeout_seconds);
        client_.set_write_timeout(config_.timeout_seconds);
        if (!config_.api_key.empty())
            client_.set_default_headers({{"api-key", config_.api_key}});
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    // Pure: documents in, upsert body out. Lets a test check the wire format
    // without a server.
    static json build_upsert_body(const std::vector<Document>& docs) {
        json points = json::array();
        for (const auto& d : docs) {
            json p;
            p["id"]     = detail::uuid_from_string(d.id);
            p["vector"] = d.embedding;
            p["payload"] = {{"content", d.content},
                            {"metadata", d.metadata.is_null() ? json::object() : d.metadata},
                            {"tiny_agent_id", d.id}};
            points.push_back(std::move(p));
        }
        return {{"points", std::move(points)}};
    }

    static std::vector<SearchResult> parse_query_response(const json& parsed) {
        // /points/query nests hits under result.points; /points/search puts them
        // straight under result. Read either.
        const json* hits = nullptr;
        if (parsed.contains("result")) {
            const auto& r = parsed["result"];
            if (r.is_object() && r.contains("points")) hits = &r["points"];
            else if (r.is_array())                     hits = &r;
        }
        if (!hits)
            throw Error("QdrantVectorStore::search: unexpected response shape: "
                + parsed.dump().substr(0, 512));

        std::vector<SearchResult> out;
        out.reserve(hits->size());
        for (const auto& hit : *hits) {
            auto payload = hit.value("payload", json::object());
            // Prefer the id we stored; fall back to whatever Qdrant echoes.
            std::string id = payload.value("tiny_agent_id", std::string{});
            if (id.empty()) {
                const auto& raw = hit["id"];
                id = raw.is_string() ? raw.get<std::string>() : raw.dump();
            }
            out.push_back({std::move(id),
                           payload.value("content", std::string{}),
                           hit.value("score", 0.0f),
                           payload.value("metadata", json::object())});
        }
        return out;
    }

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        add_batch({{id, content, embedding, metadata}});
    }

    void add_batch(const std::vector<Document>& docs) {
        if (docs.empty()) return;
        ensure_collection(static_cast<int>(docs.front().embedding.size()));

        auto body = build_upsert_body(docs);
        // wait=true so a search issued right after this call sees the points.
        auto res = client_.Put(collection_path() + "/points?wait=true",
                               body.dump(), "application/json");
        if (!res)
            throw Error("QdrantVectorStore::add failed: "
                + httplib::to_string(res.error()));
        if (res->status != 200)
            throw Error("QdrantVectorStore::add rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
        config_.log.debug("qdrant", "upserted " + std::to_string(docs.size()) + " point(s)");
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) const {
        json body;
        body["query"]        = query;    // /points/query takes "query", not "vector"
        body["limit"]        = top_k;
        body["with_payload"] = true;

        auto res = client_.Post(collection_path() + "/points/query",
                                body.dump(), "application/json");
        if (!res)
            throw Error("QdrantVectorStore::search failed: "
                + httplib::to_string(res.error()));
        if (res->status != 200)
            throw Error("QdrantVectorStore::search rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));

        return parse_query_response(parse_or_throw(res->body, "search"));
    }

    // Counted server-side rather than tracked locally, so a store another
    // process also writes to reports the truth.
    [[nodiscard]] std::size_t size() const {
        auto res = client_.Post(collection_path() + "/points/count",
                                json{{"exact", true}}.dump(), "application/json");
        if (!res || res->status != 200) return 0;   // absent collection counts as empty
        auto parsed = parse_or_throw(res->body, "size");
        if (!parsed.contains("result")) return 0;
        return parsed["result"].value("count", std::size_t{0});
    }

    void clear() {
        auto res = client_.Delete(collection_path());
        if (!res)
            throw Error("QdrantVectorStore::clear failed: "
                + httplib::to_string(res.error()));
        // A 404 means it was already gone, which is the state clear() wanted.
        if (res->status != 200 && res->status != 404)
            throw Error("QdrantVectorStore::clear rejected (status "
                + std::to_string(res->status) + "): " + res->body.substr(0, 512));
        collection_ensured_ = false;
    }

    [[nodiscard]] const std::string& collection() const { return collection_; }
};

static_assert(vector_store<QdrantVectorStore>);
static_assert(batch_vector_store<QdrantVectorStore>);

} // namespace tiny_agent
