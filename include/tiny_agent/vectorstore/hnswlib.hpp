#pragma once
#include "base.hpp"
#include <hnswlib/hnswlib.h>
#include <unordered_map>
#include <cmath>

namespace tiny_agent {

// ── HnswVectorStore — hnswlib-backed approximate nearest neighbor search ─────
//
// Uses hnswlib's HNSW index for fast O(log n) approximate search.
// Optional dependency — only include this header if you have hnswlib installed
// (add "hnswlib" to your vcpkg.json).
//
// Cosine similarity is implemented via InnerProductSpace on L2-normalized
// vectors.  Normalization is applied automatically on add() and search().

class HnswVectorStore {
    int dimensions_;
    hnswlib::InnerProductSpace space_;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> index_;
    std::unordered_map<size_t, Document> docs_;
    size_t next_label_ = 0;

    static void normalize(std::vector<float>& vec) {
        float norm = 0;
        for (float v : vec) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 0)
            for (float& v : vec) v /= norm;
    }

public:
    HnswVectorStore(int dimensions, size_t max_elements = 10000,
                    int M = 16, int ef_construction = 200)
        : dimensions_(dimensions)
        , space_(dimensions)
        , index_(std::make_unique<hnswlib::HierarchicalNSW<float>>(
              &space_, max_elements, M, ef_construction))
    {}

    HnswVectorStore(HnswVectorStore&&) = default;
    HnswVectorStore& operator=(HnswVectorStore&&) = default;

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        if (static_cast<int>(embedding.size()) != dimensions_)
            throw Error("HnswVectorStore: dimension mismatch ("
                + std::to_string(embedding.size()) + " vs "
                + std::to_string(dimensions_) + ")");

        auto norm_vec = embedding;
        normalize(norm_vec);

        auto label = next_label_++;
        index_->addPoint(norm_vec.data(), label);
        docs_[label] = {id, content, embedding, metadata};
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) const {
        if (docs_.empty()) return {};

        auto norm_query = query;
        normalize(norm_query);

        auto k = std::min(static_cast<size_t>(top_k), docs_.size());
        auto result = index_->searchKnn(norm_query.data(), k);

        std::vector<SearchResult> results;
        results.reserve(k);
        while (!result.empty()) {
            auto [dist, label] = result.top();
            result.pop();
            auto it = docs_.find(label);
            if (it != docs_.end()) {
                // hnswlib InnerProductSpace returns 1-IP as distance;
                // convert back to similarity
                float score = 1.0f - dist;
                results.push_back({it->second.id, it->second.content,
                                   score, it->second.metadata});
            }
        }
        std::reverse(results.begin(), results.end());
        return results;
    }

    [[nodiscard]] size_t size() const { return docs_.size(); }

    void clear() {
        docs_.clear();
        next_label_ = 0;
        index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            &space_, index_->max_elements_, index_->M_, index_->ef_construction_);
    }
};

static_assert(vector_store<HnswVectorStore>);

} // namespace tiny_agent
