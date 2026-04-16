#pragma once
#include "embeddings/core.hpp"
#include "vectorstore/base.hpp"
#include "vectorstore/flat.hpp"
#include "core/tool.hpp"
#include <memory>

namespace tiny_agent {

// ── Retriever<StoreType> — VectorStore + Embeddings, exposable as Agent Tool ─
//
// Owns a vector store and an AnyEmbeddings model.  Provides add_documents()
// to populate the store and query() to search it.  as_tool() returns a Tool
// that an Agent can call.
//
// Template parameter StoreType must satisfy vector_store.  Defaults to
// FlatVectorStore (brute-force cosine similarity, zero deps).
//
// Lifetime: the Tool returned by as_tool() captures `this`.  The Retriever
// must outlive any Tool (or Agent holding that Tool) created from it.
// For shared-ownership scenarios, use retriever_as_tool() with shared_ptr.
//
//   auto r = Retriever{init_embeddings("text-embedding-3-small", cfg)};
//   r.add_documents({"doc1", "doc2"});
//   auto tool = r.as_tool("search", "Search knowledge base");
//
//   // With a different store:
//   auto r2 = Retriever<HnswVectorStore>{
//       HnswVectorStore{1536, 10000}, init_embeddings(...)};

template<vector_store StoreType = FlatVectorStore>
class Retriever {
    StoreType      store_;
    AnyEmbeddings  embeddings_;
    int            default_top_k_;
    Log            log_;

public:
    Retriever(AnyEmbeddings embeddings, int top_k = 4, Log log = {})
        : embeddings_(std::move(embeddings))
        , default_top_k_(top_k)
        , log_(log) {}

    Retriever(StoreType store, AnyEmbeddings embeddings,
              int top_k = 4, Log log = {})
        : store_(std::move(store))
        , embeddings_(std::move(embeddings))
        , default_top_k_(top_k)
        , log_(log) {}

    void add_documents(const std::vector<std::string>& texts,
                       const std::vector<json>& metadata = {}) {
        log_.info("retriever",
            "adding " + std::to_string(texts.size()) + " documents");
        auto vecs = embeddings_.embed_documents(texts);
        for (size_t i = 0; i < texts.size(); ++i) {
            json meta = (i < metadata.size()) ? metadata[i] : json::object();
            std::string id = "doc_" + std::to_string(store_.size());
            store_.add(id, texts[i], vecs[i], meta);
        }
        log_.debug("retriever",
            "store now has " + std::to_string(store_.size()) + " documents");
    }

    [[nodiscard]] std::vector<SearchResult>
    query(const std::string& text, int top_k = -1) {
        if (top_k < 0) top_k = default_top_k_;
        log_.debug("retriever", "querying: \""
            + text.substr(0, 80) + "\" top_k=" + std::to_string(top_k));
        auto query_vec = embeddings_.embed_query(text);
        return store_.search(query_vec, top_k);
    }

    StoreType&       store()       { return store_; }
    const StoreType& store() const { return store_; }

    // ── Expose as Agent tool (captures this — ensure Retriever outlives Tool) ──

    Tool as_tool(std::string name, std::string description) {
        return Tool::create(std::move(name), std::move(description),
            [this](const json& args) -> json {
                auto q = args.at("query").get<std::string>();
                int k  = args.value("top_k", default_top_k_);
                auto results = query(q, k);

                json out = json::array();
                for (auto& r : results) {
                    json item;
                    item["content"] = r.content;
                    item["score"]   = r.score;
                    if (!r.metadata.empty()) item["metadata"] = r.metadata;
                    out.push_back(std::move(item));
                }
                return out;
            },
            {{"type", "object"},
             {"properties", {
                 {"query", {{"type", "string"},
                            {"description", "The search query"}}},
                 {"top_k", {{"type", "integer"},
                            {"description", "Number of results to return"}}}}},
             {"required", {"query"}}});
    }
};

// ── CTAD guides ─────────────────────────────────────────────────────────────

Retriever(AnyEmbeddings, int, Log) -> Retriever<FlatVectorStore>;
Retriever(AnyEmbeddings, int)      -> Retriever<FlatVectorStore>;
Retriever(AnyEmbeddings)           -> Retriever<FlatVectorStore>;

template<vector_store S>
Retriever(S, AnyEmbeddings, int, Log) -> Retriever<S>;
template<vector_store S>
Retriever(S, AnyEmbeddings, int)      -> Retriever<S>;
template<vector_store S>
Retriever(S, AnyEmbeddings)           -> Retriever<S>;

// ── retriever_as_tool — shared_ptr ownership for safe nesting ────────────────

template<vector_store StoreType>
Tool retriever_as_tool(std::shared_ptr<Retriever<StoreType>> retriever,
                       std::string name, std::string description) {
    return Tool::create(std::move(name), std::move(description),
        [ret = std::move(retriever)](const json& args) -> json {
            auto q = args.at("query").get<std::string>();
            int k  = args.value("top_k", 4);
            auto results = ret->query(q, k);

            json out = json::array();
            for (auto& r : results) {
                json item;
                item["content"] = r.content;
                item["score"]   = r.score;
                if (!r.metadata.empty()) item["metadata"] = r.metadata;
                out.push_back(std::move(item));
            }
            return out;
        },
        {{"type", "object"},
         {"properties", {
             {"query", {{"type", "string"},
                        {"description", "The search query"}}},
             {"top_k", {{"type", "integer"},
                        {"description", "Number of results to return"}}}}},
         {"required", {"query"}}});
}

} // namespace tiny_agent
