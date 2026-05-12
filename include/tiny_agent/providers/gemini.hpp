#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  providers/gemini.hpp  —  Full specializations of LLMModel for Google Gemini
//
//  LLMModel<Gemini, chat_tag>       — gemini-2.0-flash, gemini-pro, etc.
//  LLMModel<Gemini, embedding_tag>  — text-embedding-004, embedding-001
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/model.hpp"
#include "../core/tool.hpp"
#include <limits>
#include <memory>

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

template<> struct LLMModel<Gemini, chat_tag>;
template<> struct LLMModel<Gemini, embedding_tag>;

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<Gemini, chat_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
struct LLMModel<Gemini, chat_tag> {
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
                base_url.empty() ? "https://generativelanguage.googleapis.com" : base_url);
            log.debug("llm", "gemini client initializing (model=" + model + ")");
            client_->set_read_timeout(timeout_seconds);
            httplib::Headers hdrs;
            for (auto& [k, v] : headers) hdrs.emplace(k, v);
            if (!hdrs.empty()) client_->set_default_headers(hdrs);
#ifdef __APPLE__
            client_->set_ca_cert_path("/etc/ssl/cert.pem");
#endif
            client_init_ = true;
        }
    }

    // ── Serialization helpers ───────────────────────────────────────────────

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
                else if (p.type == "image_url" && p.image_url) {
                    auto url = p.image_url->url;
                    std::string mime = "image/jpeg";
                    auto ext = url.find_last_of('.');
                    if (ext != std::string::npos) {
                        auto suffix = url.substr(ext + 1);
                        if (suffix == "png") mime = "image/png";
                        else if (suffix == "webp") mime = "image/webp";
                        else if (suffix == "gif") mime = "image/gif";
                        else if (suffix == "bmp") mime = "image/bmp";
                        else if (suffix == "svg") mime = "image/svg+xml";
                        else if (suffix == "heic" || suffix == "heif") mime = "image/heic";
                    }
                    parts.push_back({{"fileData",
                        {{"mimeType", mime},
                         {"fileUri", url}}}});
                }
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
                       const std::vector<ToolSchema>& tools,
                       const LLMConfig& cfg) const {
        json body;

        json gen;
        if (cfg.temperature)         gen["temperature"]     = *cfg.temperature;
        if (cfg.max_tokens)          gen["maxOutputTokens"] = *cfg.max_tokens;
        if (cfg.top_p)               gen["topP"] = *cfg.top_p;
        if (cfg.top_k)               gen["topK"] = static_cast<int>(*cfg.top_k);
        if (!cfg.stop.empty())       gen["stopSequences"] = cfg.stop;
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

        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
    }

    std::string request_path() const {
        return "/v1beta/models/" + model
             + ":generateContent?key=" + api_key;
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
        self.stop = stop;
        self.response_format = response_format;
        self.thinking = thinking;
        self.timeout_seconds = timeout_seconds;
        self.headers = headers;
        self.extra = extra;
        self.log = log;

        auto cfg = overrides.api_key.empty() ? self : LLMConfig::merge(self, overrides);
        lg.debug("llm", "gemini chat (model=" + model
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
            lg.error("llm", "gemini API error (status="
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

    LLMModel& with_system(std::string sys) {
        extra["systemInstruction"] = {
            {"parts", json::array({{{"text", std::move(sys)}}})}};
        return *this;
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
//  LLMModel<Gemini, embedding_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
struct LLMModel<Gemini, embedding_tag> {
    using input_t   = std::string;
    using output_t  = std::vector<float>;
    using model_tag = embedding_tag;

    // ── Aggregate-initializable fields ──────────────────────────────────────
    std::string model;
    std::string api_key;
    std::string base_url;
    std::optional<int> dimensions_;
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
                base_url.empty() ? "https://generativelanguage.googleapis.com" : base_url);
            log.debug("embeddings", "gemini client initializing (model=" + model + ")");
            client_->set_read_timeout(timeout_seconds);
            httplib::Headers hdrs;
            for (auto& [k, v] : headers) hdrs.emplace(k, v);
            if (!hdrs.empty()) client_->set_default_headers(hdrs);
#ifdef __APPLE__
            client_->set_ca_cert_path("/etc/ssl/cert.pem");
#endif
            client_init_ = true;
        }
    }

    EmbeddingResponse embed_raw(const std::vector<std::string>& texts) {
        auto& lg = log;
        lg.debug("embeddings", "gemini embed (model=" + model
            + " texts=" + std::to_string(texts.size()) + ")");

        std::string model_path = "models/" + model;
        json requests = json::array();
        for (auto& text : texts) {
            json req;
            req["model"]   = model_path;
            req["content"] = {{"parts", json::array({{{"text", text}}})}};
            if (dimensions_)
                req["outputDimensionality"] = *dimensions_;
            requests.push_back(std::move(req));
        }
        json body;
        body["requests"] = requests;
        if (!extra.empty()) body.merge_patch(extra);

        std::string path = "/v1beta/models/" + model
             + ":batchEmbedContents?key=" + api_key;
        lg.trace("embeddings", "POST " + path);

        ensure_client();
        auto res = client_->Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            lg.error("embeddings", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            lg.error("embeddings", "gemini API error (status="
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

        lg.debug("embeddings",
            "returned " + std::to_string(embeddings.size()) + " embedding(s)");
        return {std::move(embeddings), json::object(), parsed};
    }

public:
    // ── Concept surface (is_embedding) ──────────────────────────────────────

    [[nodiscard]] std::string model_name() const { return model; }
    [[nodiscard]] std::size_t dimensions() const {
        return dimensions_ ? static_cast<std::size_t>(*dimensions_) : 0;
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

    static LLMModel from_config(std::string model, EmbeddingConfig cfg = {}) {
        LLMModel m;
        m.model = std::move(model);
        m.api_key = std::move(cfg.api_key);
        m.base_url = std::move(cfg.base_url);
        m.dimensions_ = cfg.dimensions;
        m.timeout_seconds = cfg.timeout_seconds;
        m.headers = std::move(cfg.headers);
        m.extra = std::move(cfg.extra);
        m.log = std::move(cfg.log);
        return m;
    }

    EmbeddingConfig config() const {
        EmbeddingConfig c;
        c.api_key = api_key;
        c.base_url = base_url;
        c.dimensions = dimensions_;
        c.timeout_seconds = timeout_seconds;
        c.headers = headers;
        c.extra = extra;
        c.log = log;
        return c;
    }
};

// ─── Convenience aliases ──────────────────────────────────────────────────────
using GeminiChat      = LLMModel<Gemini, chat_tag>;
using GeminiEmbedding = LLMModel<Gemini, embedding_tag>;

static_assert(is_chat<GeminiChat>,           "GeminiChat must satisfy is_chat");
static_assert(is_embedding<GeminiEmbedding>, "GeminiEmbedding must satisfy is_embedding");

} // namespace tiny_agent
