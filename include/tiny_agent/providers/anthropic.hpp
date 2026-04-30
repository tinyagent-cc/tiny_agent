#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  providers/anthropic.hpp  —  Full specialization of LLMModel for Anthropic
//
//  LLMModel<Anthropic, chat_tag>  — Claude 3/3.5/4 family
// ═══════════════════════════════════════════════════════════════════════════════

#include "../core/model.hpp"
#include "../core/tool.hpp"

namespace tiny_agent {

struct Anthropic {};

template<> class LLMModel<Anthropic, chat_tag>;

// ═══════════════════════════════════════════════════════════════════════════════
//  LLMModel<Anthropic, chat_tag>
// ═══════════════════════════════════════════════════════════════════════════════
template<>
class LLMModel<Anthropic, chat_tag> {
    std::string     model_;
    LLMConfig       config_;
    httplib::Client client_;

    void init_client() {
        config_.log.debug("llm", "anthropic client initializing (model=" + model_ + ")");
        client_.set_read_timeout(config_.timeout_seconds);
        httplib::Headers hdrs;
        hdrs.emplace("x-api-key", config_.api_key);
        hdrs.emplace("anthropic-version",
            config_.api_version.empty() ? "2023-06-01" : config_.api_version);
        for (auto& [k, v] : config_.headers) hdrs.emplace(k, v);
        if (!hdrs.empty()) client_.set_default_headers(hdrs);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

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
                       const std::vector<ToolSchema>& tools) const {
        json body;
        body["model"]       = model_;
        body["max_tokens"]  = config_.max_tokens;
        body["temperature"] = config_.temperature;

        if (config_.top_p)         body["top_p"] = *config_.top_p;
        if (config_.top_k)         body["top_k"] = static_cast<int>(*config_.top_k);
        if (!config_.stop.empty()) body["stop_sequences"] = config_.stop;

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

        if (!config_.extra.empty()) body.merge_patch(config_.extra);
        return body;
    }

public:
    using input_t   = std::string;
    using output_t  = std::string;
    using model_tag = chat_tag;

    LLMModel(std::string model, LLMConfig cfg = {})
        : model_(std::move(model))
        , config_(std::move(cfg))
        , client_(config_.base_url.empty()
              ? "https://api.anthropic.com" : config_.base_url)
    { init_client(); }

    LLMModel(std::string model, std::string api_key)
        : LLMModel(std::move(model), LLMConfig{.api_key = std::move(api_key)}) {}

    explicit LLMModel(ModelConfig cfg = {.model_name = "claude-sonnet-4-20250514"})
        : LLMModel(cfg.model_name.empty() ? "claude-sonnet-4-20250514" : cfg.model_name,
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
        log.debug("llm", "anthropic chat (model=" + model_
            + " messages=" + std::to_string(msgs.size())
            + " tools=" + std::to_string(tools.size()) + ")");

        auto body = build_request(msgs, tools);
        std::string path = "/v1/messages";
        log.trace("llm", "POST " + path);

        auto res = client_.Post(path, body.dump(), "application/json");
        if (!res) {
            auto err = "HTTP request failed: " + httplib::to_string(res.error());
            log.error("llm", err);
            throw APIError(0, err);
        }

        if (res->status != 200) {
            log.error("llm", "anthropic API error (status="
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

    const LLMConfig& config() const { return config_; }
};

using AnthropicChat = LLMModel<Anthropic, chat_tag>;

static_assert(is_chat<AnthropicChat>, "AnthropicChat must satisfy is_chat");

} // namespace tiny_agent
