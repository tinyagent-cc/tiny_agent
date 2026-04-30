#pragma once
#include "../core/model.hpp"

namespace tiny_agent {

struct VoyageAI {};

template<> class LLMModel<VoyageAI, embedding_tag>;

template<>
class LLMModel<VoyageAI, embedding_tag> {
    std::string       model_;
    EmbeddingConfig   config_;
    httplib::Client   client_;

    void init_client() {
        config_.log.debug("embeddings", "voyageai client initializing (model=" + model_ + ")");
        client_.set_read_timeout(config_.timeout_seconds);
        httplib::Headers hdrs;
        if (!config_.api_key.empty())
            hdrs.emplace("Authorization", "Bearer " + config_.api_key);
        for (auto& [k, v] : config_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    EmbeddingResponse embed_raw(const std::vector<std::string>& texts) {
        auto& log = config_.log;
        log.debug("embeddings", "voyageai embed (model=" + model_
            + " texts=" + std::to_string(texts.size()) + ")");

        json body;
        body["model"] = model_;
        body["input"] = texts;
        if (config_.dimensions) body["output_dimension"] = *config_.dimensions;
        if (!config_.extra.empty()) body.merge_patch(config_.extra);

        std::string path = "/v1/embeddings";
        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) throw APIError(0, "HTTP request failed: " + httplib::to_string(res.error()));
        if (res->status != 200)
            throw APIError(res->status, "voyageai API error: " + res->body);

        auto parsed = json::parse(res->body);
        auto& data = parsed["data"];
        std::vector<std::vector<float>> embeddings(data.size());
        for (auto& item : data) {
            auto idx = item["index"].get<size_t>();
            embeddings[idx] = item["embedding"].get<std::vector<float>>();
        }
        return {std::move(embeddings), parsed.value("usage", json::object()), parsed};
    }

public:
    using input_t   = std::string;
    using output_t  = std::vector<float>;
    using model_tag = embedding_tag;

    LLMModel(std::string model, EmbeddingConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? "https://api.voyageai.com" : config_.base_url)
    { init_client(); }

    LLMModel(std::string model, std::string api_key)
        : LLMModel(std::move(model), EmbeddingConfig{.api_key = std::move(api_key)}) {}

    LLMModel(const LLMModel&)            = delete;
    LLMModel& operator=(const LLMModel&) = delete;
    LLMModel(LLMModel&&)                 = default;
    LLMModel& operator=(LLMModel&&)      = default;

    [[nodiscard]] std::string model_name() const { return model_; }
    [[nodiscard]] std::size_t dimensions() const {
        return config_.dimensions ? static_cast<std::size_t>(*config_.dimensions) : 0;
    }

    std::vector<float> invoke(const std::string& text, const RunConfig& = {}) { return embed_query(text); }
    std::vector<float> embed_query(const std::string& text) {
        auto resp = embed_raw({text});
        if (resp.embeddings.empty()) throw Error("embed_query: no embedding returned");
        return std::move(resp.embeddings[0]);
    }
    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts) {
        if (texts.empty()) return {};
        return std::move(embed_raw(texts).embeddings);
    }

    std::vector<std::vector<float>> batch(std::vector<std::string> texts, const RunConfig& cfg = {}) {
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (auto& t : texts) out.push_back(invoke(t, cfg));
        return out;
    }
    void stream(std::string text, std::function<void(std::vector<float>)> cb, const RunConfig& cfg = {}) {
        cb(invoke(text, cfg));
    }
};

using VoyageAIEmbedding = LLMModel<VoyageAI, embedding_tag>;
static_assert(is_embedding<VoyageAIEmbedding>, "VoyageAIEmbedding must satisfy is_embedding");

} // namespace tiny_agent
