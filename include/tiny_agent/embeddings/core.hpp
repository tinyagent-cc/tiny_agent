#pragma once
#include "../core/types.hpp"
#include "../core/log.hpp"
#include <httplib.h>
#include <memory>
#include <concepts>

namespace tiny_agent {

// ── Configuration ────────────────────────────────────────────────────────────

struct EmbeddingConfig {
    std::string api_key;
    std::string base_url;

    std::optional<int> dimensions;

    int timeout_seconds = 120;
    std::map<std::string, std::string> headers;
    json extra = json::object();

    Log log;
};

// ── Response ─────────────────────────────────────────────────────────────────

struct EmbeddingResponse {
    std::vector<std::vector<float>> embeddings;
    json  usage;
    json  raw;
};

// ── Provider traits — specialize for each embedding provider ─────────────────
//
// Each specialization must provide:
//   static constexpr std::string_view name;
//   static constexpr std::string_view default_base_url;
//   static void configure_auth(httplib::Headers&, const EmbeddingConfig&);
//   static std::string request_path(std::string_view model, const EmbeddingConfig&);
//   static json build_request(std::string_view model,
//                              const std::vector<std::string>& texts,
//                              const EmbeddingConfig&);
//   static EmbeddingResponse parse_response(const json&);

template<typename Tag>
struct embedding_traits;

// ── Concept: is an embedding_traits specialization complete? ─────────────────

template<typename Tag>
concept embedding_defined = requires {
    { embedding_traits<Tag>::name } -> std::convertible_to<std::string_view>;
    { embedding_traits<Tag>::default_base_url } -> std::convertible_to<std::string_view>;
} && requires(httplib::Headers& h, const EmbeddingConfig& cfg,
              std::string_view model,
              const std::vector<std::string>& texts,
              const json& raw) {
    embedding_traits<Tag>::configure_auth(h, cfg);
    { embedding_traits<Tag>::request_path(model, cfg) } -> std::convertible_to<std::string>;
    { embedding_traits<Tag>::build_request(model, texts, cfg) } -> std::same_as<json>;
    { embedding_traits<Tag>::parse_response(raw) } -> std::same_as<EmbeddingResponse>;
};

// ── Concept: anything that quacks like an embeddings model ───────────────────

template<typename T>
concept embeddings_like = requires(T& e,
                                   const std::string& text,
                                   const std::vector<std::string>& texts) {
    { e.embed_query(text) }      -> std::same_as<std::vector<float>>;
    { e.embed_documents(texts) } -> std::same_as<std::vector<std::vector<float>>>;
    { e.model_name() }           -> std::convertible_to<std::string_view>;
};

// ── Embeddings<ProviderTag> — static-dispatch embedding client ───────────────

template<typename ProviderTag>
    requires embedding_defined<ProviderTag>
class Embeddings {
    using traits = embedding_traits<ProviderTag>;

    std::string       model_;
    EmbeddingConfig   config_;
    httplib::Client   client_;

    void init_client() {
        config_.log.debug("embeddings",
            std::string(traits::name) + " client initializing (model=" + model_ + ")");
        client_.set_read_timeout(config_.timeout_seconds);
        httplib::Headers hdrs;
        traits::configure_auth(hdrs, config_);
        for (auto& [k, v] : config_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    EmbeddingResponse embed_raw(const std::vector<std::string>& texts) {
        auto& log = config_.log;
        log.debug("embeddings", std::string(traits::name) + " embed (model=" + model_
            + " texts=" + std::to_string(texts.size()) + ")");

        auto body = traits::build_request(model_, texts, config_);
        auto path = traits::request_path(model_, config_);
        log.trace("embeddings", "POST " + path);
        log.trace("embeddings", "request: " + body.dump());

        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            log.error("embeddings", err);
            throw APIError(0, err);
        }

        log.debug("embeddings", "response status=" + std::to_string(res->status));

        if (res->status != 200) {
            log.error("embeddings", std::string(traits::name) + " API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status,
                std::string(traits::name) + " API error: " + res->body);
        }

        log.trace("embeddings", "response: " + res->body);

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string(traits::name) + " returned invalid JSON: " + e.what());
        }

        auto response = traits::parse_response(parsed);
        log.debug("embeddings",
            "returned " + std::to_string(response.embeddings.size()) + " embedding(s)");
        return response;
    }

public:
    using provider_tag = ProviderTag;

    Embeddings(std::string model, EmbeddingConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? std::string(traits::default_base_url) : config_.base_url)
    { init_client(); }

    Embeddings(std::string model, std::string api_key)
        : Embeddings(std::move(model), EmbeddingConfig{.api_key = std::move(api_key)}) {}

    Embeddings(const Embeddings&)            = delete;
    Embeddings& operator=(const Embeddings&) = delete;
    Embeddings(Embeddings&&)                 = default;
    Embeddings& operator=(Embeddings&&)      = default;

    std::vector<float> embed_query(const std::string& text) {
        auto resp = embed_raw({text});
        if (resp.embeddings.empty())
            throw Error("embed_query: no embedding returned");
        return std::move(resp.embeddings[0]);
    }

    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts) {
        if (texts.empty()) return {};
        return std::move(embed_raw(texts).embeddings);
    }

    static constexpr std::string_view provider_name() { return traits::name; }
    const std::string& model_name() const { return model_; }
    const EmbeddingConfig& config() const { return config_; }
};

// ── AnyEmbeddings — type-erased embeddings for runtime polymorphism ──────────

class AnyEmbeddings {
    struct Concept {
        virtual ~Concept() = default;
        virtual std::vector<float> do_embed_query(const std::string&) = 0;
        virtual std::vector<std::vector<float>>
            do_embed_documents(const std::vector<std::string>&) = 0;
        virtual std::string_view do_model_name() const = 0;
    };

    template<typename T>
    struct Model final : Concept {
        T impl_;
        explicit Model(T impl) : impl_(std::move(impl)) {}

        std::vector<float> do_embed_query(const std::string& text) override {
            return impl_.embed_query(text);
        }
        std::vector<std::vector<float>>
        do_embed_documents(const std::vector<std::string>& texts) override {
            return impl_.embed_documents(texts);
        }
        std::string_view do_model_name() const override {
            return impl_.model_name();
        }
    };

    std::unique_ptr<Concept> impl_;

public:
    template<embeddings_like T>
        requires (!std::same_as<std::remove_cvref_t<T>, AnyEmbeddings>)
    AnyEmbeddings(T impl) : impl_(std::make_unique<Model<T>>(std::move(impl))) {}

    AnyEmbeddings(AnyEmbeddings&&) = default;
    AnyEmbeddings& operator=(AnyEmbeddings&&) = default;

    std::vector<float> embed_query(const std::string& text) {
        return impl_->do_embed_query(text);
    }

    std::vector<std::vector<float>>
    embed_documents(const std::vector<std::string>& texts) {
        return impl_->do_embed_documents(texts);
    }

    std::string_view model_name() const { return impl_->do_model_name(); }
};

static_assert(embeddings_like<AnyEmbeddings>);

} // namespace tiny_agent
