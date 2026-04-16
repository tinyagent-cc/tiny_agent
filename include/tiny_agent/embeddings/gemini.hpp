#pragma once
#include "core.hpp"
#include "../providers/gemini.hpp"

namespace tiny_agent {

// ── Gemini Embeddings API ────────────────────────────────────────────────────
//
// POST /v1beta/models/{model}:batchEmbedContents?key=...
// Models: text-embedding-004, embedding-001

template<>
struct embedding_traits<gemini> {
    static constexpr std::string_view name             = "gemini";
    static constexpr std::string_view default_base_url =
        "https://generativelanguage.googleapis.com";

    static void configure_auth(httplib::Headers& hdrs, const EmbeddingConfig& cfg) {
        for (auto& [k, v] : cfg.headers) hdrs.emplace(k, v);
    }

    static std::string request_path(std::string_view model,
                                    const EmbeddingConfig& cfg) {
        return "/v1beta/models/" + std::string(model)
             + ":batchEmbedContents?key=" + cfg.api_key;
    }

    static json build_request(std::string_view model,
                              const std::vector<std::string>& texts,
                              const EmbeddingConfig& cfg) {
        std::string model_path = "models/" + std::string(model);
        json requests = json::array();
        for (auto& text : texts) {
            json req;
            req["model"]   = model_path;
            req["content"] = {{"parts", json::array({{{"text", text}}})}};
            if (cfg.dimensions)
                req["outputDimensionality"] = *cfg.dimensions;
            requests.push_back(std::move(req));
        }
        json body;
        body["requests"] = requests;
        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
    }

    static EmbeddingResponse parse_response(const json& j) {
        std::vector<std::vector<float>> embeddings;
        for (auto& item : j["embeddings"])
            embeddings.push_back(item["values"].get<std::vector<float>>());
        return {std::move(embeddings), json::object(), j};
    }
};

static_assert(embedding_defined<gemini>);

} // namespace tiny_agent
