#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  model.hpp  —  Model kind concepts + primary LLMModel template + type erasure
//
//  Two orthogonal model concepts:
//    is_chat      — a model that takes messages and returns an LLM response
//    is_embedding — a model that takes text and returns a float vector
//
//  LLMModel<Provider, Kind> is the primary (undefined) template, fully
//  specialized per provider x kind combination in providers/*.hpp.
//
//  ChatVariant<Ps...> / EmbeddingVariant<Ps...> provide variant-based wrappers
//  using std::visit + fold expressions — no virtual dispatch.
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.hpp"
#include "log.hpp"
#include "tool.hpp"
#include "runnable.hpp"
#include <httplib.h>
#include <concepts>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace tiny_agent {

// ─── Kind tags ────────────────────────────────────────────────────────────────

struct chat_tag      { static constexpr const char* name = "chat";      };
struct embedding_tag { static constexpr const char* name = "embedding"; };

// ─── is_chat concept ──────────────────────────────────────────────────────────
//
// A type T satisfies is_chat iff:
//   1. It declares model_tag as chat_tag
//   2. It has invoke(string) -> string          (simple Runnable surface)
//   3. It has chat(msgs, tools) -> LLMResponse  (rich agent surface)
//   4. It exposes model_name() and temperature()

template<typename T>
concept is_chat =
    requires { typename T::model_tag; }                                 &&
    std::same_as<typename T::model_tag, chat_tag>                       &&
    requires(T m, std::string prompt,
             const std::vector<Message>& msgs,
             const std::vector<ToolSchema>& tools) {
        { m.invoke(prompt)     } -> std::convertible_to<std::string>;
        { m.chat(msgs, tools)  } -> std::same_as<LLMResponse>;
        { m.model_name()       } -> std::convertible_to<std::string>;
        { m.temperature()      } -> std::convertible_to<float>;
    };

// ─── is_embedding concept ─────────────────────────────────────────────────────

template<typename T>
concept is_embedding =
    requires { typename T::model_tag; }                                 &&
    std::same_as<typename T::model_tag, embedding_tag>                  &&
    requires(T m, const std::string& text, const std::vector<std::string>& texts) {
        { m.invoke(text)           } -> std::convertible_to<std::vector<float>>;
        { m.embed_query(text)      } -> std::same_as<std::vector<float>>;
        { m.embed_documents(texts) } -> std::same_as<std::vector<std::vector<float>>>;
        { m.model_name()           } -> std::convertible_to<std::string>;
        { m.dimensions()           } -> std::convertible_to<std::size_t>;
    };

// ─── ModelConfig (simple shared config) ───────────────────────────────────────

struct ModelConfig {
    std::string  model_name   {};
    std::string  api_key      {};
    std::string  base_url     {};
    float        temperature  { 0.7f };
    std::size_t  max_tokens   { 1024 };
    std::size_t  dimensions   { 0 };
};

// ─── LLMConfig (full chat config, extends with provider-specific fields) ──────

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

// ─── EmbeddingConfig ──────────────────────────────────────────────────────────

struct EmbeddingConfig {
    std::string api_key;
    std::string base_url;

    std::optional<int> dimensions;

    int timeout_seconds = 120;
    std::map<std::string, std::string> headers;
    json extra = json::object();

    Log log;
};

// ─── EmbeddingResponse ────────────────────────────────────────────────────────

struct EmbeddingResponse {
    std::vector<std::vector<float>> embeddings;
    json  usage;
    json  raw;
};

// ─── Primary LLMModel template (undefined — forces specialization) ────────────

template<typename Provider, typename Kind>
class LLMModel;

// ─── ChatVariant — variant-based wrapper for is_chat models ──────────────────
//
// Uses std::variant<LLMModel<Providers, chat_tag>...> with std::visit for
// dispatch.  No virtual functions, no heap allocation.  The variant is
// instantiated only when the provider specializations are visible (typically
// in init_chat_model.hpp).  Satisfies is_chat.

template<typename... Providers>
class ChatVariant {
    std::variant<LLMModel<Providers, chat_tag>...> v_;

public:
    using model_tag = chat_tag;
    using input_t   = std::string;
    using output_t  = std::string;

    template<typename T>
        requires (!std::same_as<std::remove_cvref_t<T>, ChatVariant>) &&
                 (std::same_as<std::remove_cvref_t<T>, LLMModel<Providers, chat_tag>> || ...)
    ChatVariant(T&& impl) : v_(std::forward<T>(impl)) {}

    ChatVariant(ChatVariant&&) = default;
    ChatVariant& operator=(ChatVariant&&) = default;

    std::string invoke(std::string prompt, const RunConfig& = {}) {
        return std::visit([&](auto& m) -> std::string {
            return m.invoke(std::move(prompt));
        }, v_);
    }

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {}) {
        return std::visit([&](auto& m) -> LLMResponse {
            return m.chat(msgs, tools);
        }, v_);
    }

    std::string model_name() const {
        return std::visit([](const auto& m) -> std::string {
            return std::string(m.model_name());
        }, v_);
    }

    float temperature() const {
        return std::visit([](const auto& m) -> float {
            return m.temperature();
        }, v_);
    }

    std::vector<std::string> batch(std::vector<std::string> prompts, const RunConfig& cfg = {}) {
        std::vector<std::string> out;
        out.reserve(prompts.size());
        for (auto& p : prompts) out.push_back(invoke(std::move(p), cfg));
        return out;
    }

    void stream(std::string prompt, std::function<void(std::string)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(prompt), cfg));
    }
};

// ─── EmbeddingVariant — variant-based wrapper for is_embedding models ────────

template<typename... Providers>
class EmbeddingVariant {
    std::variant<LLMModel<Providers, embedding_tag>...> v_;

public:
    using model_tag = embedding_tag;
    using input_t   = std::string;
    using output_t  = std::vector<float>;

    template<typename T>
        requires (!std::same_as<std::remove_cvref_t<T>, EmbeddingVariant>) &&
                 (std::same_as<std::remove_cvref_t<T>, LLMModel<Providers, embedding_tag>> || ...)
    EmbeddingVariant(T&& impl) : v_(std::forward<T>(impl)) {}

    EmbeddingVariant(EmbeddingVariant&&) = default;
    EmbeddingVariant& operator=(EmbeddingVariant&&) = default;

    std::vector<float> invoke(const std::string& text, const RunConfig& = {}) {
        return std::visit([&](auto& m) -> std::vector<float> {
            return m.invoke(text);
        }, v_);
    }

    std::vector<float> embed_query(const std::string& text) {
        return std::visit([&](auto& m) -> std::vector<float> {
            return m.embed_query(text);
        }, v_);
    }

    std::vector<std::vector<float>>
    embed_documents(const std::vector<std::string>& texts) {
        return std::visit([&](auto& m) -> std::vector<std::vector<float>> {
            return m.embed_documents(texts);
        }, v_);
    }

    std::string model_name() const {
        return std::visit([](const auto& m) -> std::string {
            return std::string(m.model_name());
        }, v_);
    }

    std::size_t dimensions() const {
        return std::visit([](const auto& m) -> std::size_t {
            return m.dimensions();
        }, v_);
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

} // namespace tiny_agent
