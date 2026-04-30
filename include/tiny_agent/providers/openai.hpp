#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  providers/openai.hpp  —  Full specializations of LLMModel for OpenAI
//
//  LLMModel<OpenAI, chat_tag>       — GPT-4o, GPT-4-turbo, etc.
//  LLMModel<OpenAI, embedding_tag>  — text-embedding-3-{small,large}
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/model.hpp"
#include "../core/tool.hpp"

namespace tiny_agent {

struct OpenAI {};

template<> class LLMModel<OpenAI, chat_tag>;
template<> class LLMModel<OpenAI, embedding_tag>;

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<OpenAI, chat_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
class LLMModel<OpenAI, chat_tag> {
    std::string     model_;
    LLMConfig       config_;
    httplib::Client client_;

    void init_client() {
        config_.log.debug("llm", "openai client initializing (model=" + model_ + ")");
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

    // ── Serialization helpers ────────────────────────────────────────────────

    static json message_to_json(const Message& m) {
        json j;
        j["role"] = to_string(m.role);

        if (auto* s = std::get_if<std::string>(&m.content)) {
            j["content"] = *s;
        } else if (auto* parts = std::get_if<std::vector<ContentPart>>(&m.content)) {
            json arr = json::array();
            for (auto& p : *parts) {
                if (p.type == "text") {
                    arr.push_back({{"type", "text"}, {"text", p.text}});
                } else if (p.type == "image_url" && p.image_url) {
                    arr.push_back({{"type", "image_url"},
                        {"image_url", {{"url", p.image_url->url},
                                       {"detail", p.image_url->detail}}}});
                }
            }
            j["content"] = arr;
        }

        if (!m.tool_calls.empty()) {
            json tcs = json::array();
            for (auto& tc : m.tool_calls) {
                tcs.push_back({
                    {"id", tc.id}, {"type", "function"},
                    {"function", {{"name", tc.name},
                                  {"arguments", tc.arguments.dump()}}}
                });
            }
            j["tool_calls"] = tcs;
        }

        if (m.tool_call_id) j["tool_call_id"] = *m.tool_call_id;
        if (m.name)         j["name"] = *m.name;
        return j;
    }

    static json tool_schema_to_json(const ToolSchema& ts) {
        return {{"type", "function"},
                {"function", {{"name", ts.name},
                              {"description", ts.description},
                              {"parameters", ts.parameters}}}};
    }

    static Message parse_choice(const json& choice) {
        auto& msg = choice["message"];
        Message m;
        m.role = role_from_string(msg.value("role", std::string{"assistant"}));
        m.content = (msg.contains("content") && !msg["content"].is_null())
            ? msg["content"].get<std::string>() : std::string{};

        if (msg.contains("tool_calls"))
            for (auto& tc : msg["tool_calls"])
                m.tool_calls.push_back({
                    tc["id"].get<std::string>(),
                    tc["function"]["name"].get<std::string>(),
                    json::parse(tc["function"]["arguments"].get<std::string>())
                });
        return m;
    }

    json build_request(const std::vector<Message>& messages,
                       const std::vector<ToolSchema>& tools) const {
        json body;
        body["model"]       = model_;
        body["temperature"] = config_.temperature;
        body["max_tokens"]  = config_.max_tokens;

        if (config_.top_p)              body["top_p"]              = *config_.top_p;
        if (config_.frequency_penalty)  body["frequency_penalty"]  = *config_.frequency_penalty;
        if (config_.presence_penalty)   body["presence_penalty"]   = *config_.presence_penalty;
        if (config_.seed)               body["seed"]               = *config_.seed;
        if (!config_.stop.empty())      body["stop"]               = config_.stop;
        if (config_.response_format)
            body["response_format"] = {{"type", *config_.response_format}};

        json msgs = json::array();
        for (auto& m : messages)
            msgs.push_back(message_to_json(m));
        body["messages"] = msgs;

        if (!tools.empty()) {
            json ts = json::array();
            for (auto& t : tools)
                ts.push_back(tool_schema_to_json(t));
            body["tools"] = ts;
        }

        if (!config_.extra.empty()) body.merge_patch(config_.extra);
        return body;
    }

    std::string request_path() const {
        std::string path = "/v1/chat/completions";
        if (!config_.api_version.empty())
            path += "?api-version=" + config_.api_version;
        return path;
    }

public:
    using input_t   = std::string;
    using output_t  = std::string;
    using model_tag = chat_tag;

    LLMModel(std::string model, LLMConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? "https://api.openai.com" : config_.base_url)
    { init_client(); }

    LLMModel(std::string model, std::string api_key)
        : LLMModel(std::move(model), LLMConfig{.api_key = std::move(api_key)}) {}

    explicit LLMModel(ModelConfig cfg = {.model_name = "gpt-4o"})
        : LLMModel(cfg.model_name.empty() ? "gpt-4o" : cfg.model_name,
                    LLMConfig{.api_key = cfg.api_key,
                              .base_url = cfg.base_url,
                              .temperature = cfg.temperature,
                              .max_tokens = static_cast<int>(cfg.max_tokens)}) {}

    LLMModel(const LLMModel&)            = delete;
    LLMModel& operator=(const LLMModel&) = delete;
    LLMModel(LLMModel&&)                 = default;
    LLMModel& operator=(LLMModel&&)      = default;

    // ── Concept surface (is_chat) ───────────────────────────────────────────

    [[nodiscard]] std::string model_name()  const { return model_; }
    [[nodiscard]] float       temperature() const { return static_cast<float>(config_.temperature); }

    std::string invoke(std::string prompt, const RunConfig& = {}) {
        std::vector<Message> msgs = {Message::user(std::move(prompt))};
        auto resp = chat(msgs);
        return resp.message.text();
    }

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {}) {
        auto& log = config_.log;
        log.debug("llm", "openai chat (model=" + model_
            + " messages=" + std::to_string(msgs.size())
            + " tools=" + std::to_string(tools.size()) + ")");

        auto body = build_request(msgs, tools);
        auto path = request_path();
        log.trace("llm", "POST " + path);

        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            log.error("llm", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            log.error("llm", "openai API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status, "openai API error: " + res->body);
        }

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string("openai returned invalid JSON: ") + e.what());
        }

        auto& choice = parsed["choices"][0];
        LLMResponse response{
            parse_choice(choice),
            parsed.value("usage", json::object()),
            choice.value("finish_reason", std::string{}),
            parsed
        };

        log.debug("llm", "finish_reason=" + response.finish_reason
            + " tool_calls=" + std::to_string(response.message.tool_calls.size()));
        return response;
    }

    // ── Runnable surface ────────────────────────────────────────────────────

    std::vector<std::string> batch(std::vector<std::string> prompts, const RunConfig& cfg = {}) {
        std::vector<std::string> out;
        out.reserve(prompts.size());
        for (auto& p : prompts) out.push_back(invoke(std::move(p), cfg));
        return out;
    }

    void stream(std::string prompt, std::function<void(std::string)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(prompt), cfg));
    }

    const LLMConfig& config() const { return config_; }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<OpenAI, embedding_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
class LLMModel<OpenAI, embedding_tag> {
    std::string       model_;
    EmbeddingConfig   config_;
    httplib::Client   client_;

    void init_client() {
        config_.log.debug("embeddings", "openai client initializing (model=" + model_ + ")");
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
        log.debug("embeddings", "openai embed (model=" + model_
            + " texts=" + std::to_string(texts.size()) + ")");

        json body;
        body["model"]           = model_;
        body["input"]           = texts;
        body["encoding_format"] = "float";
        if (config_.dimensions) body["dimensions"] = *config_.dimensions;
        if (!config_.extra.empty()) body.merge_patch(config_.extra);

        std::string path = "/v1/embeddings";
        log.trace("embeddings", "POST " + path);

        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            log.error("embeddings", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            log.error("embeddings", "openai API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status, "openai API error: " + res->body);
        }

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string("openai returned invalid JSON: ") + e.what());
        }

        auto& data = parsed["data"];
        std::vector<std::vector<float>> embeddings(data.size());
        for (auto& item : data) {
            auto idx = item["index"].get<size_t>();
            embeddings[idx] = item["embedding"].get<std::vector<float>>();
        }

        log.debug("embeddings",
            "returned " + std::to_string(embeddings.size()) + " embedding(s)");
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
              ? "https://api.openai.com" : config_.base_url)
    { init_client(); }

    LLMModel(std::string model, std::string api_key)
        : LLMModel(std::move(model), EmbeddingConfig{.api_key = std::move(api_key)}) {}

    explicit LLMModel(ModelConfig cfg = {.model_name = "text-embedding-3-large", .dimensions = 3072})
        : LLMModel(cfg.model_name.empty() ? "text-embedding-3-large" : cfg.model_name,
                    EmbeddingConfig{.api_key = cfg.api_key,
                                   .base_url = cfg.base_url,
                                   .dimensions = cfg.dimensions ? std::optional<int>(static_cast<int>(cfg.dimensions)) : std::nullopt}) {}

    LLMModel(const LLMModel&)            = delete;
    LLMModel& operator=(const LLMModel&) = delete;
    LLMModel(LLMModel&&)                 = default;
    LLMModel& operator=(LLMModel&&)      = default;

    // ── Concept surface (is_embedding) ──────────────────────────────────────

    [[nodiscard]] std::string model_name() const { return model_; }
    [[nodiscard]] std::size_t dimensions() const {
        return config_.dimensions ? static_cast<std::size_t>(*config_.dimensions) : 0;
    }

    std::vector<float> invoke(const std::string& text, const RunConfig& = {}) {
        return embed_query(text);
    }

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

    // ── Runnable surface ────────────────────────────────────────────────────

    std::vector<std::vector<float>> batch(std::vector<std::string> texts, const RunConfig& cfg = {}) {
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (auto& t : texts) out.push_back(invoke(t, cfg));
        return out;
    }

    void stream(std::string text, std::function<void(std::vector<float>)> cb, const RunConfig& cfg = {}) {
        cb(invoke(text, cfg));
    }

    const EmbeddingConfig& config() const { return config_; }
};

// ─── Convenience aliases ──────────────────────────────────────────────────────
using OpenAIChat      = LLMModel<OpenAI, chat_tag>;
using OpenAIEmbedding = LLMModel<OpenAI, embedding_tag>;

static_assert(is_chat<OpenAIChat>,           "OpenAIChat must satisfy is_chat");
static_assert(is_embedding<OpenAIEmbedding>, "OpenAIEmbedding must satisfy is_embedding");

} // namespace tiny_agent
