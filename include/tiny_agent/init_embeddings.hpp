#pragma once
#include "embeddings/core.hpp"
#include "embeddings/openai.hpp"
#include "embeddings/gemini.hpp"
#include "embeddings/mistral.hpp"
#include "embeddings/cohere.hpp"
#include "embeddings/voyageai.hpp"

namespace tiny_agent {

// ── Embedding model-string parsing ───────────────────────────────────────────
//
// Accepts "provider:model" (explicit) or just "model" (auto-detected).
//   "openai:text-embedding-3-small"  → provider=openai
//   "gemini:text-embedding-004"      → provider=gemini
//   "mistral:mistral-embed"          → provider=mistral
//   "cohere:embed-v4"                → provider=cohere
//   "voyageai:voyage-4"              → provider=voyageai
//   "text-embedding-3-small"         → provider=openai  (auto-detected)
//   "mistral-embed"                  → provider=mistral (auto-detected)
//   "my-custom-model"                → provider=openai  (fallback — supports
//                                      OpenAI-compatible APIs like vLLM, Ollama)

struct EmbeddingModelSpec {
    std::string provider;
    std::string model;
};

inline EmbeddingModelSpec parse_embedding_model_string(const std::string& model_string) {
    auto colon = model_string.find(':');
    if (colon != std::string::npos && colon > 0 && colon < model_string.size() - 1)
        return {model_string.substr(0, colon), model_string.substr(colon + 1)};

    auto starts = [&](std::string_view prefix) {
        return model_string.size() >= prefix.size() &&
               model_string.compare(0, prefix.size(), prefix) == 0;
    };

    if (starts("text-embedding-"))      return {"openai",  model_string};
    if (starts("embedding-") ||
        starts("text-multilingual-"))   return {"gemini",  model_string};
    if (starts("mistral-embed"))        return {"mistral", model_string};
    if (starts("embed-"))               return {"cohere",  model_string};
    if (starts("voyage-"))              return {"voyageai", model_string};

    return {"openai", model_string};
}

// ── init_embeddings — provider-agnostic embedding factory ────────────────────
//
// Returns AnyEmbeddings (type-erased) so the caller doesn't need to know the
// concrete provider at compile time.  Mirrors LangChain's init_embeddings().
//
//   auto emb = init_embeddings("openai:text-embedding-3-small",
//       EmbeddingConfig{.api_key = getenv("OPENAI_API_KEY")});

inline AnyEmbeddings init_embeddings(const std::string& model_string,
                                     EmbeddingConfig config = {}) {
    auto [provider, model] = parse_embedding_model_string(model_string);

    if (provider == "openai")
        return AnyEmbeddings{Embeddings<openai>{model, std::move(config)}};
    if (provider == "gemini")
        return AnyEmbeddings{Embeddings<gemini>{model, std::move(config)}};
    if (provider == "mistral")
        return AnyEmbeddings{Embeddings<mistral>{model, std::move(config)}};
    if (provider == "cohere")
        return AnyEmbeddings{Embeddings<cohere>{model, std::move(config)}};
    if (provider == "voyageai")
        return AnyEmbeddings{Embeddings<voyageai>{model, std::move(config)}};

    throw Error("init_embeddings: unknown provider '" + provider +
                "' (supported: openai, gemini, mistral, cohere, voyageai)");
}

// Convenience: create from explicit provider + model strings.
inline AnyEmbeddings init_embeddings(const std::string& provider,
                                     const std::string& model,
                                     EmbeddingConfig config = {}) {
    return init_embeddings(provider + ":" + model, std::move(config));
}

} // namespace tiny_agent
