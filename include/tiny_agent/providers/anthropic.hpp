#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  providers/anthropic.hpp  —  Full specialization of LLMModel for Anthropic
//
//  LLMModel<Anthropic, chat_tag>  — Claude 3/3.5/4 family
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/model.hpp"
#include "../core/tool.hpp"
#include <limits>
#include <memory>

namespace tiny_agent {

struct Anthropic {};

template<> struct LLMModel<Anthropic, chat_tag>;

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<Anthropic, chat_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
struct LLMModel<Anthropic, chat_tag> {
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
                base_url.empty() ? "https://api.anthropic.com" : base_url);
            log.debug("llm", "anthropic client initializing (model=" + model + ")");
            client_->set_read_timeout(timeout_seconds);
            httplib::Headers hdrs;
            if (!api_key.empty())
                hdrs.emplace("x-api-key", api_key);
            hdrs.emplace("anthropic-version",
                api_version.empty() ? "2023-06-01" : api_version);
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
        j["role"] = (m.role == Role::assistant) ? "assistant" : "user";

        if (m.role == Role::tool) {
            j["role"] = "user";
            j["content"] = json::array({{
                {"type", "tool_result"},
                {"tool_use_id", m.tool_call_id.value_or("")},
                {"content", m.text()}
            }});
            return j;
        }

        if (auto* parts = std::get_if<std::vector<ContentPart>>(&m.content)) {
            json arr = json::array();
            for (auto& p : *parts) {
                if (p.type == "text") {
                    arr.push_back({{"type", "text"}, {"text", p.text}});
                } else if (p.type == "image_url" && p.image_url) {
                    arr.push_back({{"type", "image"},
                        {"source", {{"type", "url"}, {"url", p.image_url->url}}}});
                }
            }
            j["content"] = arr;
        } else {
            j["content"] = std::get<std::string>(m.content);
        }

        if (!m.tool_calls.empty()) {
            j["role"] = "assistant";
            json blocks = json::array();
            auto txt = m.text();
            if (!txt.empty())
                blocks.push_back({{"type", "text"}, {"text", txt}});
            for (auto& tc : m.tool_calls)
                blocks.push_back({{"type", "tool_use"}, {"id", tc.id},
                                  {"name", tc.name}, {"input", tc.arguments}});
            j["content"] = blocks;
        }
        return j;
    }

    static json tool_schema_to_json(const ToolSchema& ts) {
        return {{"name", ts.name},
                {"description", ts.description},
                {"input_schema", ts.parameters}};
    }

    static Message parse_message(const json& j) {
        Message m;
        m.role = Role::assistant;
        std::string text;
        for (auto& block : j["content"]) {
            if (block["type"] == "text")
                text += block["text"].get<std::string>();
            else if (block["type"] == "tool_use")
                m.tool_calls.push_back({
                    block["id"].get<std::string>(),
                    block["name"].get<std::string>(),
                    block["input"]
                });
        }
        m.content = std::move(text);
        return m;
    }

    json build_request(const std::vector<Message>& messages,
                       const std::vector<ToolSchema>& tools,
                       const LLMConfig& cfg) const {
        json body;
        body["model"] = model;
        body["max_tokens"]  = cfg.max_tokens.value_or(4096);
        if (cfg.temperature)         body["temperature"] = *cfg.temperature;
        if (cfg.top_p)               body["top_p"] = *cfg.top_p;
        if (cfg.top_k)               body["top_k"] = static_cast<int>(*cfg.top_k);
        if (!cfg.stop.empty())       body["stop_sequences"] = cfg.stop;
        if (cfg.response_format)
            body["response_format"] = {{"type", *cfg.response_format}};

        json msgs = json::array();
        std::string merged_system;
        for (auto& m : messages) {
            if (m.role == Role::system) {
                if (!merged_system.empty()) merged_system += "\n\n";
                merged_system += m.text();
                continue;
            }
            msgs.push_back(message_to_json(m));
        }
        if (!merged_system.empty()) body["system"] = merged_system;
        body["messages"] = msgs;

        if (!tools.empty()) {
            json ts = json::array();
            for (auto& t : tools)
                ts.push_back(tool_schema_to_json(t));
            body["tools"] = ts;
        }

        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
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
        lg.debug("llm", "anthropic chat (model=" + model
            + " messages=" + std::to_string(msgs.size())
            + " tools=" + std::to_string(tools.size()) + ")");

        auto body = build_request(msgs, tools, cfg);
        std::string path = "/v1/messages";
        lg.trace("llm", "POST " + path);

        ensure_client();
        auto res = client_->Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            lg.error("llm", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            lg.error("llm", "anthropic API error (status="
                + std::to_string(res->status) + "): " + res->body);
            throw APIError(res->status, "anthropic API error: " + res->body);
        }

        json parsed;
        try {
            parsed = json::parse(res->body);
        } catch (const std::exception& e) {
            throw APIError(res->status,
                std::string("anthropic returned invalid JSON: ") + e.what());
        }

        LLMResponse response{
            parse_message(parsed),
            parsed.value("usage", json::object()),
            parsed.value("stop_reason", std::string{}),
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

// ─── Convenience alias ─────────────────────────────────────────────────────────
using AnthropicChat = LLMModel<Anthropic, chat_tag>;

static_assert(is_chat<AnthropicChat>, "AnthropicChat must satisfy is_chat");

} // namespace tiny_agent
