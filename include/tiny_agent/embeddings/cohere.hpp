#pragma once
#include "core.hpp"

namespace tiny_agent {

// ── Provider tag ────────────────────────────────────────────────────────────

struct cohere {};

// ── Cohere Embed API v2 ──────────────────────────────────────────────────────
//
// POST https://api.cohere.com/v2/embed
// Models: embed-v4, embed-multilingual-v3.0, embed-english-v3.0
// Requires embedding_types field; returns embeddings keyed by type.

template<>
struct embedding_traits<cohere> {
    static constexpr std::string_view name             = "cohere";
    static constexpr std::string_view default_base_url = "https://api.cohere.com";

    static void configure_auth(httplib::Headers& hdrs, const EmbeddingConfig& cfg) {
        if (!cfg.api_key.empty())
            hdrs.emplace("Authorization", "Bearer " + cfg.api_key);
    }

    static std::string request_path(std::string_view /*model*/,
                                    const EmbeddingConfig& /*cfg*/) {
        return "/v2/embed";
    }

    static json build_request(std::string_view model,
                              const std::vector<std::string>& texts,
                              const EmbeddingConfig& cfg) {
        json body;
        body["model"]           = model;
        body["texts"]           = texts;
        body["input_type"]      = "search_document";
        body["embedding_types"] = json::array({"float"});
        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
    }

    static EmbeddingResponse parse_response(const json& j) {
        std::vector<std::vector<float>> embeddings;
        auto& floats = j["embeddings"]["float"];
        for (auto& vec : floats)
            embeddings.push_back(vec.get<std::vector<float>>());
        return {std::move(embeddings), j.value("meta", json::object()), j};
    }
};

static_assert(embedding_defined<cohere>);

} // namespace tiny_agent
