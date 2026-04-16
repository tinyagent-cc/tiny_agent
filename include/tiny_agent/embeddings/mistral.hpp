#pragma once
#include "core.hpp"

namespace tiny_agent {

// ── Provider tag ────────────────────────────────────────────────────────────

struct mistral {};

// ── Mistral Embeddings API ───────────────────────────────────────────────────
//
// POST https://api.mistral.ai/v1/embeddings
// Models: mistral-embed (1024 dims)
// OpenAI-compatible format with minor differences (output_dimension).

template<>
struct embedding_traits<mistral> {
    static constexpr std::string_view name             = "mistral";
    static constexpr std::string_view default_base_url = "https://api.mistral.ai";

    static void configure_auth(httplib::Headers& hdrs, const EmbeddingConfig& cfg) {
        if (!cfg.api_key.empty())
            hdrs.emplace("Authorization", "Bearer " + cfg.api_key);
    }

    static std::string request_path(std::string_view /*model*/,
                                    const EmbeddingConfig& /*cfg*/) {
        return "/v1/embeddings";
    }

    static json build_request(std::string_view model,
                              const std::vector<std::string>& texts,
                              const EmbeddingConfig& cfg) {
        json body;
        body["model"]           = model;
        body["input"]           = texts;
        body["encoding_format"] = "float";
        if (cfg.dimensions) body["output_dimension"] = *cfg.dimensions;
        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
    }

    static EmbeddingResponse parse_response(const json& j) {
        auto& data = j["data"];
        std::vector<std::vector<float>> embeddings(data.size());
        for (auto& item : data) {
            auto idx = item["index"].get<size_t>();
            embeddings[idx] = item["embedding"].get<std::vector<float>>();
        }
        return {std::move(embeddings), j.value("usage", json::object()), j};
    }
};

static_assert(embedding_defined<mistral>);

} // namespace tiny_agent
