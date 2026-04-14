#pragma once
#include "types.hpp"
#include "tool.hpp"
#include "log.hpp"
#include <httplib.h>
#include <memory>
#include <concepts>

namespace tiny_agent {

// ── Configuration (runtime values: api keys, temperatures, etc.) ────────────

struct LLMConfig {
    std::string api_key;
    std::string base_url;
    std::string api_version;

    double temperature  = 0.7;
    int    max_tokens   = 4096;
    std::optional<double> top_p;
    std::optional<double> top_k;
    std::optional<double> frequency_penalty;
    std::optional<double> presence_penalty;
    std::optional<int>    seed;
    std::vector<std::string> stop;
    std::optional<std::string> response_format;

    int timeout_seconds = 120;
    std::map<std::string, std::string> headers;
    json extra = json::object();

    Log log;
};

// ── Provider traits — specialize for each provider ──────────────────────────
//
// Each specialization must provide:
//   static constexpr std::string_view name;
//   static constexpr std::string_view default_base_url;
//   static void configure_auth(httplib::Headers&, const LLMConfig&);
//   static std::string request_path(std::string_view model, const LLMConfig&);
//   static json build_request(std::string_view model,
//                              const std::vector<Message>&,
//                              const std::vector<ToolSchema>&,
//                              const LLMConfig&);
//   static LLMResponse parse_response(const json&);

template<typename Tag>
struct provider_traits;

// ── Concept: is a provider_traits specialization complete? ──────────────────

template<typename Tag>
concept provider_defined = requires {
    { provider_traits<Tag>::name } -> std::convertible_to<std::string_view>;
    { provider_traits<Tag>::default_base_url } -> std::convertible_to<std::string_view>;
} && requires(httplib::Headers& h, const LLMConfig& cfg,
              std::string_view model,
              const std::vector<Message>& msgs,
              const std::vector<ToolSchema>& tools,
              const json& raw) {
    provider_traits<Tag>::configure_auth(h, cfg);
    { provider_traits<Tag>::request_path(model, cfg) } -> std::convertible_to<std::string>;
    { provider_traits<Tag>::build_request(model, msgs, tools, cfg) } -> std::same_as<json>;
    { provider_traits<Tag>::parse_response(raw) } -> std::same_as<LLMResponse>;
};

// ── Concept: anything that quacks like an LLM ───────────────────────────────

template<typename T>
concept llm_like = requires(T& llm,
                            const std::vector<Message>& msgs,
                            const std::vector<ToolSchema>& tools) {
    { llm.chat(msgs, tools) } -> std::same_as<LLMResponse>;
    { llm.model_name() } -> std::convertible_to<std::string_view>;
};

// ── LLM<ProviderTag> — static-dispatch, zero virtual overhead ───────────────

template<typename ProviderTag>
    requires provider_defined<ProviderTag>
class LLM {
    using traits = provider_traits<ProviderTag>;

    std::string     model_;
    LLMConfig       config_;
    httplib::Client  client_;

    void init_client() {
        config_.log.debug("llm", std::string(traits::name) + " client initializing (model=" + model_ + ")");
        client_.set_read_timeout(config_.timeout_seconds);
        httplib::Headers hdrs;
        traits::configure_auth(hdrs, config_);
        for (auto& [k, v] : config_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
        auto base = config_.base_url.empty()
            ? std::string(traits::default_base_url) : config_.base_url;
        config_.log.trace("llm", "base_url=" + base + " timeout=" + std::to_string(config_.timeout_seconds) + "s");
    }

public:
    using provider_tag = ProviderTag;

    LLM(std::string model, LLMConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? std::string(traits::default_base_url) : config_.base_url)
    { init_client(); }

    LLM(std::string model, std::string api_key)
        : LLM(std::move(model), LLMConfig{.api_key = std::move(api_key)}) {}

    LLM(const LLM&)            = delete;
    LLM& operator=(const LLM&) = delete;
    LLM(LLM&&)                 = default;
    LLM& operator=(LLM&&)      = default;

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {}) {
        auto& log = config_.log;
        log.debug("llm", std::string(traits::name) + " chat (model=" + model_
            + " messages=" + std::to_string(msgs.size())
            + " tools=" + std::to_string(tools.size()) + ")");

        auto body = traits::build_request(model_, msgs, tools, config_);
        auto path = traits::request_path(model_, config_);
        log.trace("llm", "POST " + path);
        log.trace("llm", "request: " + body.dump());

        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            log.error("llm", err);
            throw APIError(0, err);
        }

        log.debug("llm", "response status=" + std::to_string(res->status));

        if (res->status != 200) {
            log.error("llm", std::string(traits::name) + " API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status,
                std::string(traits::name) + " API error: " + res->body);
        }

        log.trace("llm", "response: " + res->body);

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string(traits::name) + " returned invalid JSON: " + e.what());
        }
        auto response = traits::parse_response(parsed);

        log.debug("llm", "finish_reason=" + response.finish_reason
            + " tool_calls=" + std::to_string(response.message.tool_calls.size()));
        if (!response.usage.empty())
            log.debug("llm", "usage: " + response.usage.dump());

        return response;
    }

    template<typename Parser>
    auto chat_parsed(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {})
        -> typename Parser::output_type
    {
        return Parser::parse(chat(msgs, tools));
    }

    static constexpr std::string_view provider_name() { return traits::name; }
    const std::string& model_name() const { return model_; }
    const LLMConfig& config() const { return config_; }
};

// ── AnyLLM — type-erased LLM for runtime polymorphism when needed ───────────
//
// Use this for heterogeneous agent composition (mixing providers at runtime).
// Satisfies llm_like so Agent<AnyLLM> works seamlessly.

class AnyLLM {
    struct Concept {
        virtual ~Concept() = default;
        virtual LLMResponse do_chat(const std::vector<Message>&,
                                    const std::vector<ToolSchema>&) = 0;
        virtual std::string_view do_model_name() const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;
    };

    template<typename T>
    struct Model final : Concept {
        T impl_;
        explicit Model(T impl) : impl_(std::move(impl)) {}

        LLMResponse do_chat(const std::vector<Message>& m,
                           const std::vector<ToolSchema>& t) override {
            return impl_.chat(m, t);
        }
        std::string_view do_model_name() const override {
            return impl_.model_name();
        }
        std::unique_ptr<Concept> clone() const override {
            if constexpr (std::copy_constructible<T>)
                return std::make_unique<Model>(*this);
            else
                throw Error("AnyLLM: wrapped type is not copyable");
        }
    };

    std::unique_ptr<Concept> impl_;

public:
    template<llm_like T>
        requires (!std::same_as<std::remove_cvref_t<T>, AnyLLM>)
    AnyLLM(T impl) : impl_(std::make_unique<Model<T>>(std::move(impl))) {}

    AnyLLM(AnyLLM&&) = default;
    AnyLLM& operator=(AnyLLM&&) = default;

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {}) {
        return impl_->do_chat(msgs, tools);
    }

    std::string_view model_name() const { return impl_->do_model_name(); }
};

static_assert(llm_like<AnyLLM>);

} // namespace tiny_agent
