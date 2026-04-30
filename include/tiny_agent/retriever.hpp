#pragma once
#include "core/model.hpp"
#include "vectorstore/base.hpp"
#include "vectorstore/flat.hpp"
#include "core/tool.hpp"
#include <memory>

namespace tiny_agent {

template<is_embedding EmbeddingType, vector_store StoreType = FlatVectorStore>
class Retriever {
    StoreType      store_;
    EmbeddingType  embeddings_;
    int            default_top_k_;
    Log            log_;

public:
    Retriever(EmbeddingType embeddings, int top_k = 4, Log log = {})
        : embeddings_(std::move(embeddings))
        , default_top_k_(top_k)
        , log_(log) {}

    Retriever(StoreType store, EmbeddingType embeddings,
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

    DynamicTool as_tool(std::string name, std::string description) {
        return DynamicTool::create(std::move(name), std::move(description),
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

// CTAD guides
template<is_embedding E>
Retriever(E, int, Log) -> Retriever<E, FlatVectorStore>;
template<is_embedding E>
Retriever(E, int)      -> Retriever<E, FlatVectorStore>;
template<is_embedding E>
Retriever(E)           -> Retriever<E, FlatVectorStore>;

template<is_embedding E, vector_store S>
Retriever(S, E, int, Log) -> Retriever<E, S>;
template<is_embedding E, vector_store S>
Retriever(S, E, int)      -> Retriever<E, S>;
template<is_embedding E, vector_store S>
Retriever(S, E)           -> Retriever<E, S>;

template<is_embedding EmbeddingType, vector_store StoreType>
DynamicTool retriever_as_tool(std::shared_ptr<Retriever<EmbeddingType, StoreType>> retriever,
                              std::string name, std::string description) {
    return DynamicTool::create(std::move(name), std::move(description),
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
