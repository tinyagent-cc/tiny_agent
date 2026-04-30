#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  providers/gemini.hpp  —  Full specializations of LLMModel for Google Gemini
//
//  LLMModel<Gemini, chat_tag>       — gemini-2.0-flash, gemini-pro, etc.
//  LLMModel<Gemini, embedding_tag>  — text-embedding-004, embedding-001
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/model.hpp"
#include "../core/tool.hpp"

namespace tiny_agent {

struct Gemini {};

enum class GeminiTaskType {
    RetrievalDocument,
    RetrievalQuery,
    SemanticSimilarity,
    Classification,
    Clustering,
};

struct GeminiChatConfig {
    std::string model               { "gemini-2.0-flash" };
    std::string system_instruction  {};
    std::string api_key             {};
    float       temperature         { 0.7f };
    std::size_t max_output_tokens   { 2048 };
};

template<> class LLMModel<Gemini, chat_tag>;
template<> class LLMModel<Gemini, embedding_tag>;

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<Gemini, chat_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
class LLMModel<Gemini, chat_tag> {
    std::string     model_;
    LLMConfig       config_;
    httplib::Client client_;

    void init_client() {
        config_.log.debug("llm", "gemini client initializing (model=" + model_ + ")");
        client_.set_read_timeout(config_.timeout_seconds);
        httplib::Headers hdrs;
        for (auto& [k, v] : config_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    static json to_gemini_type(const json& schema) {
        json out = schema;
        if (out.contains("type") && out["type"].is_string()) {
            auto t = out["type"].get<std::string>();
            for (auto& c : t)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            out["type"] = t;
        }
        if (out.contains("properties"))
            for (auto& [k, v] : out["properties"].items())
                out["properties"][k] = to_gemini_type(v);
        if (out.contains("items"))
            out["items"] = to_gemini_type(out["items"]);
        return out;
    }

    static json message_to_json(const Message& m) {
        json parts = json::array();

        if (m.role == Role::tool) {
            json resp;
            try { resp = json::parse(m.text()); } catch (...) { resp = m.text(); }
            if (!resp.is_object()) resp = json{{"result", resp}};

            parts.push_back({{"functionResponse",
                {{"name", m.name.value_or("")}, {"response", resp}}}});
            return {{"role", "user"}, {"parts", parts}};
        }

        if (auto* cp = std::get_if<std::vector<ContentPart>>(&m.content)) {
            for (auto& p : *cp) {
                if (p.type == "text")
                    parts.push_back({{"text", p.text}});
                else if (p.type == "image_url" && p.image_url)
                    parts.push_back({{"fileData",
                        {{"mimeType", "image/jpeg"},
                         {"fileUri", p.image_url->url}}}});
            }
        } else {
            auto txt = std::get<std::string>(m.content);
            if (!txt.empty()) parts.push_back({{"text", txt}});
        }

        for (auto& tc : m.tool_calls)
            parts.push_back({{"functionCall",
                {{"name", tc.name}, {"args", tc.arguments}}}});

        std::string role = (m.role == Role::assistant) ? "model" : "user";
        return {{"role", role}, {"parts", parts}};
    }

    static json tool_schema_to_json(const ToolSchema& ts) {
        json decl;
        decl["name"]        = ts.name;
        decl["description"] = ts.description;
        if (!ts.parameters.empty())
            decl["parameters"] = to_gemini_type(ts.parameters);
        return decl;
    }

    static Message parse_candidate(const json& candidate) {
        Message m;
        m.role = Role::assistant;
        std::string text;

        if (candidate.contains("content") && candidate["content"].contains("parts"))
            for (auto& part : candidate["content"]["parts"]) {
                if (part.contains("text"))
                    text += part["text"].get<std::string>();
                else if (part.contains("functionCall")) {
                    auto& fc = part["functionCall"];
                    m.tool_calls.push_back({
                        "gemini_tc_" + std::to_string(m.tool_calls.size()),
                        fc["name"].get<std::string>(),
                        fc.value("args", json::object())
                    });
                }
            }

        m.content = std::move(text);
        return m;
    }

    json build_request(const std::vector<Message>& messages,
                       const std::vector<ToolSchema>& tools) const {
        json body;

        json gen;
        gen["temperature"]     = config_.temperature;
        gen["maxOutputTokens"] = config_.max_tokens;
        if (config_.top_p) gen["topP"] = *config_.top_p;
        if (config_.top_k) gen["topK"] = static_cast<int>(*config_.top_k);
        if (!config_.stop.empty()) gen["stopSequences"] = config_.stop;
        body["generationConfig"] = gen;

        json contents = json::array();
        std::string merged_system;
        for (auto& m : messages) {
            if (m.role == Role::system) {
                if (!merged_system.empty()) merged_system += "\n\n";
                merged_system += m.text();
                continue;
            }
            contents.push_back(message_to_json(m));
        }
        if (!merged_system.empty())
            body["systemInstruction"] = {
                {"parts", json::array({{{"text", merged_system}}})}};
        body["contents"] = contents;

        if (!tools.empty()) {
            json decls = json::array();
            for (auto& t : tools) decls.push_back(tool_schema_to_json(t));
            body["tools"] = json::array({{{"functionDeclarations", decls}}});
        }

        if (!config_.extra.empty()) body.merge_patch(config_.extra);
        return body;
    }

    std::string request_path() const {
        return "/v1beta/models/" + model_
             + ":generateContent?key=" + config_.api_key;
    }

public:
    using input_t   = std::string;
    using output_t  = std::string;
    using model_tag = chat_tag;
    using Config    = GeminiChatConfig;

    LLMModel(std::string model, LLMConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? "https://generativelanguage.googleapis.com" : config_.base_url)
    { init_client(); }

    LLMModel(std::string model, std::string api_key)
        : LLMModel(std::move(model), LLMConfig{.api_key = std::move(api_key)}) {}

    explicit LLMModel(GeminiChatConfig cfg = GeminiChatConfig{})
        : LLMModel(std::move(cfg.model),
                    LLMConfig{.api_key = std::move(cfg.api_key),
                              .temperature = cfg.temperature,
                              .max_tokens = static_cast<int>(cfg.max_output_tokens)}) {}

    explicit LLMModel(ModelConfig cfg)
        : LLMModel(cfg.model_name.empty() ? "gemini-2.0-flash" : cfg.model_name,
                    LLMConfig{.api_key = cfg.api_key,
                              .base_url = cfg.base_url,
                              .temperature = cfg.temperature,
                              .max_tokens = static_cast<int>(cfg.max_tokens)}) {}

    LLMModel(const LLMModel&)            = delete;
    LLMModel& operator=(const LLMModel&) = delete;
    LLMModel(LLMModel&&)                 = default;
    LLMModel& operator=(LLMModel&&)      = default;

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
        log.debug("llm", "gemini chat (model=" + model_
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
            log.error("llm", "gemini API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status, "gemini API error: " + res->body);
        }

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string("gemini returned invalid JSON: ") + e.what());
        }

        if (!parsed.contains("candidates") || parsed["candidates"].empty())
            throw APIError(0, "Gemini returned no candidates: " + parsed.dump());

        auto& candidate = parsed["candidates"][0];
        LLMResponse response{
            parse_candidate(candidate),
            parsed.value("usageMetadata", json::object()),
            candidate.value("finishReason", std::string{}),
            parsed
        };

        log.debug("llm", "finish_reason=" + response.finish_reason
            + " tool_calls=" + std::to_string(response.message.tool_calls.size()));
        return response;
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

    LLMModel& with_system(std::string sys) {
        config_.extra["systemInstruction"] = {
            {"parts", json::array({{{"text", std::move(sys)}}})}};
        return *this;
    }

    const LLMConfig& config() const { return config_; }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<Gemini, embedding_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
class LLMModel<Gemini, embedding_tag> {
    std::string       model_;
    EmbeddingConfig   config_;
    httplib::Client   client_;

    void init_client() {
        config_.log.debug("embeddings", "gemini client initializing (model=" + model_ + ")");
        client_.set_read_timeout(config_.timeout_seconds);
        httplib::Headers hdrs;
        for (auto& [k, v] : config_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    EmbeddingResponse embed_raw(const std::vector<std::string>& texts) {
        auto& log = config_.log;
        log.debug("embeddings", "gemini embed (model=" + model_
            + " texts=" + std::to_string(texts.size()) + ")");

        std::string model_path = "models/" + model_;
        json requests = json::array();
        for (auto& text : texts) {
            json req;
            req["model"]   = model_path;
            req["content"] = {{"parts", json::array({{{"text", text}}})}};
            if (config_.dimensions)
                req["outputDimensionality"] = *config_.dimensions;
            requests.push_back(std::move(req));
        }
        json body;
        body["requests"] = requests;
        if (!config_.extra.empty()) body.merge_patch(config_.extra);

        std::string path = "/v1beta/models/" + model_
             + ":batchEmbedContents?key=" + config_.api_key;
        log.trace("embeddings", "POST " + path);

        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            log.error("embeddings", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            log.error("embeddings", "gemini API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status, "gemini API error: " + res->body);
        }

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string("gemini returned invalid JSON: ") + e.what());
        }

        std::vector<std::vector<float>> embeddings;
        for (auto& item : parsed["embeddings"])
            embeddings.push_back(item["values"].get<std::vector<float>>());

        log.debug("embeddings",
            "returned " + std::to_string(embeddings.size()) + " embedding(s)");
        return {std::move(embeddings), json::object(), parsed};
    }

public:
    using input_t   = std::string;
    using output_t  = std::vector<float>;
    using model_tag = embedding_tag;

    LLMModel(std::string model, EmbeddingConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? "https://generativelanguage.googleapis.com" : config_.base_url)
    { init_client(); }

    LLMModel(std::string model, std::string api_key)
        : LLMModel(std::move(model), EmbeddingConfig{.api_key = std::move(api_key)}) {}

    explicit LLMModel(ModelConfig cfg = {.model_name = "text-embedding-004", .dimensions = 768})
        : LLMModel(cfg.model_name.empty() ? "text-embedding-004" : cfg.model_name,
                    EmbeddingConfig{.api_key = cfg.api_key,
                                   .base_url = cfg.base_url,
                                   .dimensions = cfg.dimensions ? std::optional<int>(static_cast<int>(cfg.dimensions)) : std::nullopt}) {}

    LLMModel(const LLMModel&)            = delete;
    LLMModel& operator=(const LLMModel&) = delete;
    LLMModel(LLMModel&&)                 = default;
    LLMModel& operator=(LLMModel&&)      = default;

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

using GeminiChat      = LLMModel<Gemini, chat_tag>;
using GeminiEmbedding = LLMModel<Gemini, embedding_tag>;

static_assert(is_chat<GeminiChat>,           "GeminiChat must satisfy is_chat");
static_assert(is_embedding<GeminiEmbedding>, "GeminiEmbedding must satisfy is_embedding");

} // namespace tiny_agent
