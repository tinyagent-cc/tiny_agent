#pragma once
#include "../core/types.hpp"
#include <string>
#include <vector>
#include <concepts>

namespace tiny_agent {

// ── Document & search result ─────────────────────────────────────────────────

struct Document {
    std::string          id;
    std::string          content;
    std::vector<float>   embedding;
    json                 metadata;
};

struct SearchResult {
    std::string  id;
    std::string  content;
    float        score;
    json         metadata;
};

// ── Concept: anything that quacks like a vector store ────────────────────────
//
// Minimal interface for a vector store.  Implement this concept to plug in
// any backend: in-memory brute-force, hnswlib ANN, Qdrant, FAISS, etc.

template<typename T>
concept vector_store = requires(T& s,
                                const std::string& id,
                                const std::string& content,
                                const std::vector<float>& embedding,
                                const json& metadata,
                                int top_k) {
    s.add(id, content, embedding, metadata);
    { s.search(embedding, top_k) } -> std::same_as<std::vector<SearchResult>>;
    { s.size() }                   -> std::convertible_to<std::size_t>;
    s.clear();
};

} // namespace tiny_agent
