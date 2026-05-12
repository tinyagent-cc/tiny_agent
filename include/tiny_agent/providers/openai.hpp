#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  providers/openai.hpp  —  Full specializations of LLMModel for OpenAI
//
//  LLMModel<OpenAI, chat_tag>       — GPT-4o, GPT-4-turbo, etc.
//  LLMModel<OpenAI, embedding_tag>  — text-embedding-3-{small,large}
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/model.hpp"
#include "../core/tool.hpp"
#include <limits>
#include <memory>

namespace tiny_agent {

struct OpenAI {};

template<> struct LLMModel<OpenAI, chat_tag>;
template<> class LLMModel<OpenAI, embedding_tag>;

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<OpenAI, chat_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
struct LLMModel<OpenAI, chat_tag> {
    using input_t   = std::string;
    using output_t  = std::string;
    using model_tag = chat_tag;

    // ── Aggregate-initializable fields ──────────────────────────────────────
    std::string model;
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

    // implementation detail (public only for C++20 aggregate init)
    mutable std::unique_ptr<httplib::Client> client_;
    mutable bool client_init_ = false;

private:

    void ensure_client() const {
        if (!client_init_) {
            client_ = std::make_unique<httplib::Client>(
                base_url.empty() ? "https://api.openai.com" : base_url);
            log.debug("llm", "openai client initializing (model=" + model + ")");
            client_->set_read_timeout(timeout_seconds);
            httplib::Headers hdrs;
            if (!api_key.empty())
                hdrs.emplace("Authorization", "Bearer " + api_key);
            for (auto& [k, v] : headers) hdrs.emplace(k, v);
            if (!hdrs.empty()) client_->set_default_headers(hdrs);
#ifdef __APPLE__
            client_->set_ca_cert_path("/etc/ssl/cert.pem");
#endif
            client_init_ = true;
        }
    }

    // ── Serialization helpers ───────────────────────────────────────────────

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
                       const std::vector<ToolSchema>& tools,
                       const LLMConfig& cfg) const {
        json body;
        body["model"]       = model;
        if (cfg.temperature)         body["temperature"] = *cfg.temperature;
        if (cfg.max_tokens)          body["max_tokens"]  = *cfg.max_tokens;
        if (cfg.top_p)               body["top_p"]       = *cfg.top_p;
        if (cfg.top_k)               body["top_k"]       = *cfg.top_k;
        if (cfg.frequency_penalty)   body["frequency_penalty"] = *cfg.frequency_penalty;
        if (cfg.presence_penalty)    body["presence_penalty"]  = *cfg.presence_penalty;
        if (cfg.seed)                body["seed"]        = *cfg.seed;
        if (!cfg.stop.empty())       body["stop"]        = cfg.stop;
        if (cfg.response_format)
            body["response_format"] = {{"type", *cfg.response_format}};

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

        if (!extra.empty()) body.merge_patch(extra);
        return body;
    }

    std::string request_path() const {
        std::string path = "/v1/chat/completions";
        if (!api_version.empty())
            path += "?api-version=" + api_version;
        return path;
    }

public:
    // ── Concept surface (is_chat) ───────────────────────────────────────────

    [[nodiscard]] std::string model_name()  const { return model; }
    [[nodiscard]] float       get_temperature() const { return static_cast<float>(temperature.value_or(0.7)); }

    std::string invoke(std::string prompt, const RunConfig& = {}) {
        std::vector<Message> msgs = {Message::user(std::move(prompt))};
        auto resp = chat(msgs);
        return resp.message.text();
    }

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& tools = {},
                     const LLMConfig& overrides = {}) {
        auto& lg = log;
        LLMConfig self;
        self.api_key = api_key;
        self.base_url = base_url;
        self.api_version = api_version;
        self.temperature = temperature;
        self.max_tokens = max_tokens;
        self.top_p = top_p;
        self.top_k = top_k;
        self.frequency_penalty = frequency_penalty;
        self.presence_penalty = presence_penalty;
        self.seed = seed;
        self.stop = stop;
        self.response_format = response_format;
        self.thinking = thinking;
        self.timeout_seconds = timeout_seconds;
        self.headers = headers;
        self.extra = extra;
        self.log = log;

        auto cfg = overrides.api_key.empty() ? self : LLMConfig::merge(self, overrides);
        lg.debug("llm", "openai chat (model=" + model
            + " messages=" + std::to_string(msgs.size())
            + " tools=" + std::to_string(tools.size()) + ")");

        auto body = build_request(msgs, tools, cfg);
        auto path = request_path();
        lg.trace("llm", "POST " + path);

        ensure_client();
        auto res = client_->Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            lg.error("llm", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            lg.error("llm", "openai API error (status="
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

        lg.debug("llm", "finish_reason=" + response.finish_reason
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

    static LLMModel from_config(std::string model, LLMConfig cfg) {
        LLMModel m;
        m.model = std::move(model);
        m.api_key = std::move(cfg.api_key);
        m.base_url = std::move(cfg.base_url);
        m.api_version = std::move(cfg.api_version);
        m.temperature = cfg.temperature;
        m.max_tokens = cfg.max_tokens;
        m.top_p = cfg.top_p;
        m.top_k = cfg.top_k;
        m.frequency_penalty = cfg.frequency_penalty;
        m.presence_penalty = cfg.presence_penalty;
        m.seed = cfg.seed;
        m.stop = std::move(cfg.stop);
        m.response_format = std::move(cfg.response_format);
        m.thinking = cfg.thinking;
        m.timeout_seconds = cfg.timeout_seconds;
        m.headers = std::move(cfg.headers);
        m.extra = std::move(cfg.extra);
        m.log = std::move(cfg.log);
        return m;
    }

    LLMConfig config() const {
        LLMConfig c;
        c.api_key = api_key;
        c.base_url = base_url;
        c.api_version = api_version;
        c.temperature = temperature;
        c.max_tokens = max_tokens;
        c.top_p = top_p;
        c.top_k = top_k;
        c.frequency_penalty = frequency_penalty;
        c.presence_penalty = presence_penalty;
        c.seed = seed;
        c.stop = stop;
        c.response_format = response_format;
        c.thinking = thinking;
        c.timeout_seconds = timeout_seconds;
        c.headers = headers;
        c.extra = extra;
        c.log = log;
        return c;
    }
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
            if (idx >= embeddings.size())
                throw Error("openai embed: index " + std::to_string(idx)
                    + " out of range (size=" + std::to_string(embeddings.size()) + ")");
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
