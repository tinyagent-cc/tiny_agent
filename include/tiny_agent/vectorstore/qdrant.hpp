#pragma once
#include "base.hpp"
#include "../core/log.hpp"
#include <httplib.h>

namespace tiny_agent {

// ── QdrantConfig ─────────────────────────────────────────────────────────────

struct QdrantConfig {
    std::string api_key;
    int         timeout_seconds = 30;
    Log         log;
};

// ── QdrantVectorStore — HTTP client to a Qdrant server ───────────────────────
//
// Uses Qdrant's REST API for persistent, scalable vector search.
// Requires a running Qdrant instance.  No new C++ dependencies beyond httplib
// (already part of tiny_agent).
//
//   auto store = QdrantVectorStore{"http://localhost:6333", "my_docs",
//       QdrantConfig{.api_key = "..."}};
//   store.add("id1", "content", embedding, metadata);
//   auto results = store.search(query_vec, 5);

class QdrantVectorStore {
    std::string      base_url_;
    std::string      collection_;
    QdrantConfig     config_;
    httplib::Client  client_;
    bool             collection_ensured_ = false;
    int              dimensions_         = 0;
    size_t           size_               = 0;

    void ensure_collection(int dims) {
        if (collection_ensured_) return;
        dimensions_ = dims;

        auto path = "/collections/" + collection_;
        auto res = client_.Get(path);
        if (res && res->status == 200) {
            collection_ensured_ = true;
            auto body = json::parse(res->body);
            if (body.contains("result") && body["result"].contains("points_count"))
                size_ = body["result"]["points_count"].get<size_t>();
            return;
        }

        config_.log.info("qdrant", "creating collection '" + collection_
            + "' (dims=" + std::to_string(dims) + ")");

        json body;
        body["vectors"] = {{"size", dims}, {"distance", "Cosine"}};
        auto create_res = client_.Put(path, body.dump(), "application/json");
        if (!create_res || (create_res->status != 200 && create_res->status != 409))
            throw Error("QdrantVectorStore: failed to create collection '"
                + collection_ + "': "
                + (create_res ? create_res->body : "connection failed"));
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
        client_.set_read_timeout(config_.timeout_seconds);
        if (!config_.api_key.empty())
            client_.set_default_headers({{"api-key", config_.api_key}});
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        ensure_collection(static_cast<int>(embedding.size()));

        json point;
        point["id"]      = id;
        point["vector"]  = embedding;
        point["payload"] = {{"content", content}, {"metadata", metadata}};

        json body;
        body["points"] = json::array({point});

        auto path = "/collections/" + collection_ + "/points?wait=true";
        auto res = client_.Put(path, body.dump(), "application/json");
        if (!res || res->status != 200)
            throw Error("QdrantVectorStore::add failed: "
                + (res ? res->body : "connection failed"));
        ++size_;
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) const {
        json body;
        body["vector"]      = query;
        body["limit"]       = top_k;
        body["with_payload"] = true;

        auto path = "/collections/" + collection_ + "/points/search";
        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res || res->status != 200)
            throw Error("QdrantVectorStore::search failed: "
                + (res ? res->body : "connection failed"));

        auto parsed = json::parse(res->body);
        std::vector<SearchResult> results;
        for (auto& hit : parsed["result"]) {
            auto& payload = hit["payload"];
            results.push_back({
                hit["id"].get<std::string>(),
                payload.value("content", std::string{}),
                hit["score"].get<float>(),
                payload.value("metadata", json::object())
            });
        }
        return results;
    }

    [[nodiscard]] size_t size() const { return size_; }

    void clear() {
        json body;
        body["vectors"] = {{"size", dimensions_}, {"distance", "Cosine"}};

        auto path = "/collections/" + collection_;
        client_.Delete(path);
        client_.Put(path, body.dump(), "application/json");
        size_ = 0;
    }
};

static_assert(vector_store<QdrantVectorStore>);

} // namespace tiny_agent
