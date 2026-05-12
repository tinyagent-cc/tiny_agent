#pragma once
#include "types.hpp"
#include "log.hpp"
#include "tool.hpp"
#include "runnable.hpp"
#include <httplib.h>
#include <concepts>
#include <functional>
#include <string>
#include <map>
#include <limits>
#include <variant>

namespace tiny_agent {

struct chat_tag      { static constexpr const char* name = "chat";      };
struct embedding_tag { static constexpr const char* name = "embedding"; };

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
        { m.get_temperature()  } -> std::convertible_to<float>;
    };

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

struct ModelConfig {
    std::string  model_name   {};
    std::string  api_key      {};
    std::string  base_url     {};
    float        temperature  { 0.7f };
    std::size_t  max_tokens   { 1024 };
    std::size_t  dimensions   { 0 };
};

struct LLMConfig {
    std::string api_key;
    std::string base_url;
    std::string api_version;

    std::optional<double> temperature;
    std::optional<int>    max_tokens;
    std::optional<double> top_p;
    std::optional<double> top_k;
    std::optional<double> frequency_penalty;
    std::optional<double> presence_penalty;
    std::optional<int>    seed;
    std::vector<std::string> stop;
    std::optional<std::string> response_format;
    std::optional<bool>   thinking;

    int timeout_seconds = 120;
    std::map<std::string, std::string> headers;
    json extra = json::object();

    Log log;

    static LLMConfig merge(const LLMConfig& base, const LLMConfig& overrides) {
        if (overrides.api_key.size()) return overrides;
        LLMConfig c = base;
        if (!overrides.base_url.empty())            c.base_url = overrides.base_url;
        if (!overrides.api_version.empty())         c.api_version = overrides.api_version;
        if (overrides.temperature)                  c.temperature = overrides.temperature;
        if (overrides.max_tokens)                   c.max_tokens = overrides.max_tokens;
        if (overrides.top_p)                        c.top_p = overrides.top_p;
        if (overrides.top_k)                        c.top_k = overrides.top_k;
        if (overrides.frequency_penalty)            c.frequency_penalty = overrides.frequency_penalty;
        if (overrides.presence_penalty)             c.presence_penalty = overrides.presence_penalty;
        if (overrides.seed)                         c.seed = overrides.seed;
        if (!overrides.stop.empty())                c.stop = overrides.stop;
        if (overrides.response_format)              c.response_format = overrides.response_format;
        if (overrides.thinking)                     c.thinking = overrides.thinking;
        if (overrides.timeout_seconds != 120)       c.timeout_seconds = overrides.timeout_seconds;
        for (auto& [k, v] : overrides.headers)      c.headers[k] = v;
        if (!overrides.extra.empty())               c.extra.merge_patch(overrides.extra);
        return c;
    }
};

struct EmbeddingConfig {
    std::string api_key;
    std::string base_url;

    std::optional<int> dimensions;

    int timeout_seconds = 120;
    std::map<std::string, std::string> headers;
    json extra = json::object();

    Log log;
};

struct EmbeddingResponse {
    std::vector<std::vector<float>> embeddings;
    json  usage;
    json  raw;
};

template<typename Provider, typename Kind>
class LLMModel;

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

    float get_temperature() const {
        return std::visit([](const auto& m) -> float { return m.get_temperature(); }, v_);
    }

    std::vector<std::string> batch(std::vector<std::string> prompts, const RunConfig& = {}) {
        std::vector<std::string> out;
        out.reserve(prompts.size());
        for (auto& p : prompts) out.push_back(invoke(std::move(p)));
        return out;
    }

    void stream(std::string prompt, std::function<void(std::string)> cb, const RunConfig& = {}) {
        cb(invoke(std::move(prompt)));
    }
};

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
        return std::visit([&](auto& m) -> std::vector<float> { return m.invoke(text); }, v_);
    }

    std::vector<float> embed_query(const std::string& text) {
        return std::visit([&](auto& m) -> std::vector<float> { return m.embed_query(text); }, v_);
    }

    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts) {
        return std::visit([&](auto& m) -> std::vector<std::vector<float>> {
            return m.embed_documents(texts);
        }, v_);
    }

    std::string model_name() const {
        return std::visit([](const auto& m) -> std::string { return std::string(m.model_name()); }, v_);
    }

    std::size_t dimensions() const {
        return std::visit([](const auto& m) -> std::size_t { return m.dimensions(); }, v_);
    }

    std::vector<std::vector<float>> batch(std::vector<std::string> texts, const RunConfig& = {}) {
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (auto& t : texts) out.push_back(invoke(t));
        return out;
    }

    void stream(std::string text, std::function<void(std::vector<float>)> cb, const RunConfig& = {}) {
        cb(invoke(text));
    }
};

} // namespace tiny_agent
