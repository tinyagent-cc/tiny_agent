#pragma once
#include "../core/types.hpp"
#include <memory>
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
    float        score;      // higher is closer, whatever metric the backend uses
    json         metadata;
};

// ── Concept: anything that quacks like a vector store ────────────────────────
//
// The whole interface. Implement these four to plug in any backend: in-memory
// brute force, hnswlib, Qdrant, Chroma, something of your own.
//
//   add(id, content, embedding, metadata)  store one document
//   search(embedding, top_k)               nearest neighbours, best first
//   size()                                 how many documents are stored
//   clear()                                drop them all
//
// search() returns a *similarity*: higher means closer. Backends that natively
// report a distance convert before returning, so a Retriever reads the same way
// against every store.

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

// A store that can also take a batch in one call. REST-backed stores implement
// this to collapse N round trips into one; callers reach it through
// add_documents() below, which falls back to a loop for stores without it.
template<typename T>
concept batch_vector_store = vector_store<T> &&
    requires(T& s, const std::vector<Document>& docs) {
        s.add_batch(docs);
    };

template<vector_store S>
void add_documents(S& store, const std::vector<Document>& docs) {
    if constexpr (batch_vector_store<S>) {
        store.add_batch(docs);
    } else {
        for (const auto& d : docs)
            store.add(d.id, d.content, d.embedding, d.metadata);
    }
}

// ── AnyVectorStore — the same interface, chosen at runtime ───────────────────
//
// The concept is resolved at compile time, which is what you want when the
// backend is known. When it comes from a config file instead, wrap the concrete
// store in one of these and the calling code stops caring which it got.
//
//   AnyVectorStore store = cfg.backend == "qdrant"
//       ? AnyVectorStore{QdrantVectorStore{cfg.url, "docs"}}
//       : AnyVectorStore{FlatVectorStore{}};

class AnyVectorStore {
    struct Iface {
        virtual ~Iface() = default;
        virtual void add(const std::string&, const std::string&,
                         const std::vector<float>&, const json&) = 0;
        virtual void add_batch(const std::vector<Document>&) = 0;
        virtual std::vector<SearchResult> search(const std::vector<float>&, int) = 0;
        virtual std::size_t size() const = 0;
        virtual void clear() = 0;
    };

    template<vector_store S>
    struct Impl final : Iface {
        S store;
        explicit Impl(S s) : store(std::move(s)) {}

        void add(const std::string& id, const std::string& content,
                 const std::vector<float>& emb, const json& meta) override {
            store.add(id, content, emb, meta);
        }
        void add_batch(const std::vector<Document>& docs) override {
            add_documents(store, docs);
        }
        std::vector<SearchResult> search(const std::vector<float>& q, int k) override {
            return store.search(q, k);
        }
        std::size_t size() const override { return store.size(); }
        void clear() override { store.clear(); }
    };

    std::shared_ptr<Iface> impl_;

public:
    template<vector_store S>
        requires (!std::same_as<std::remove_cvref_t<S>, AnyVectorStore>)
    AnyVectorStore(S store)   // NOLINT(google-explicit-constructor): the point is implicit wrapping
        : impl_(std::make_shared<Impl<std::remove_cvref_t<S>>>(std::move(store))) {}

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        impl_->add(id, content, embedding, metadata);
    }

    void add_batch(const std::vector<Document>& docs) { impl_->add_batch(docs); }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) {
        return impl_->search(query, top_k);
    }

    [[nodiscard]] std::size_t size() const { return impl_->size(); }
    void clear() { impl_->clear(); }
};

static_assert(vector_store<AnyVectorStore>);

} // namespace tiny_agent
