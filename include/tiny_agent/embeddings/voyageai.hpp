#pragma once
#include "core.hpp"

namespace tiny_agent {

// ── Provider tag ────────────────────────────────────────────────────────────

struct voyageai {};

// ── Voyage AI Embeddings API ─────────────────────────────────────────────────
//
// POST https://api.voyageai.com/v1/embeddings
// Models: voyage-4-large, voyage-4, voyage-4-lite, voyage-code-3
// OpenAI-compatible format.  Anthropic's recommended embedding provider.

template<>
struct embedding_traits<voyageai> {
    static constexpr std::string_view name             = "voyageai";
    static constexpr std::string_view default_base_url = "https://api.voyageai.com";

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
        body["model"] = model;
        body["input"] = texts;
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

static_assert(embedding_defined<voyageai>);

} // namespace tiny_agent
