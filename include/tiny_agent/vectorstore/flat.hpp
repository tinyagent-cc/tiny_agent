#pragma once
#include "base.hpp"
#include <algorithm>
#include <cmath>

namespace tiny_agent {

// ── FlatVectorStore — brute-force cosine-similarity search ───────────────────
//
// O(n) scan over all documents.  Zero external dependencies.
// Good for small-to-medium corpora (hundreds to low thousands of docs).

class FlatVectorStore {
    std::vector<Document> docs_;
    int next_id_ = 0;

    static float cosine_similarity(const std::vector<float>& a,
                                   const std::vector<float>& b) {
        if (a.size() != b.size())
            throw Error("cosine_similarity: dimension mismatch ("
                + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")");
        float dot = 0, na = 0, nb = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            na  += a[i] * a[i];
            nb  += b[i] * b[i];
        }
        float denom = std::sqrt(na) * std::sqrt(nb);
        return denom > 0 ? dot / denom : 0.0f;
    }

public:
    void add(std::string content, std::vector<float> embedding,
             json metadata = json::object()) {
        std::string id = "doc_" + std::to_string(next_id_++);
        docs_.push_back({std::move(id), std::move(content),
                         std::move(embedding), std::move(metadata)});
    }

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        docs_.push_back({id, content, embedding, metadata});
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) const {
        std::vector<SearchResult> results;
        results.reserve(docs_.size());
        for (auto& doc : docs_)
            results.push_back({doc.id, doc.content,
                               cosine_similarity(query, doc.embedding),
                               doc.metadata});

        auto n = std::min(static_cast<int>(results.size()), top_k);
        std::partial_sort(results.begin(), results.begin() + n, results.end(),
            [](auto& a, auto& b) { return a.score > b.score; });
        results.resize(static_cast<size_t>(n));
        return results;
    }

    [[nodiscard]] size_t size() const { return docs_.size(); }
    void clear() { docs_.clear(); next_id_ = 0; }
};

static_assert(vector_store<FlatVectorStore>);

} // namespace tiny_agent
