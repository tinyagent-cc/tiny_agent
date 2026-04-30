#pragma once
#include "core/model.hpp"
#include "providers/openai.hpp"
#include "providers/gemini.hpp"
#include "providers/mistral.hpp"
#include "providers/cohere.hpp"
#include "providers/voyageai.hpp"

namespace tiny_agent {

using AnyEmbedding = EmbeddingVariant<OpenAI, Gemini, Mistral, Cohere, VoyageAI>;
static_assert(is_embedding<AnyEmbedding>);

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

inline AnyEmbedding init_embeddings(const std::string& model_string,
                                    EmbeddingConfig config = {}) {
    auto [provider, model] = parse_embedding_model_string(model_string);

    if (provider == "openai")
        return AnyEmbedding{LLMModel<OpenAI, embedding_tag>{model, std::move(config)}};
    if (provider == "gemini")
        return AnyEmbedding{LLMModel<Gemini, embedding_tag>{model, std::move(config)}};
    if (provider == "mistral")
        return AnyEmbedding{LLMModel<Mistral, embedding_tag>{model, std::move(config)}};
    if (provider == "cohere")
        return AnyEmbedding{LLMModel<Cohere, embedding_tag>{model, std::move(config)}};
    if (provider == "voyageai")
        return AnyEmbedding{LLMModel<VoyageAI, embedding_tag>{model, std::move(config)}};

    throw Error("init_embeddings: unknown provider '" + provider +
                "' (supported: openai, gemini, mistral, cohere, voyageai)");
}

inline AnyEmbedding init_embeddings(const std::string& provider,
                                    const std::string& model,
                                    EmbeddingConfig config = {}) {
    return init_embeddings(provider + ":" + model, std::move(config));
}

} // namespace tiny_agent
