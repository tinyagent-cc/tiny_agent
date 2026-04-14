#pragma once
#include "../core/llm.hpp"

namespace tiny_agent {

// ── Provider tag ────────────────────────────────────────────────────────────

struct anthropic {};

// ── Traits specialization ───────────────────────────────────────────────────

template<>
struct provider_traits<anthropic> {
    static constexpr std::string_view name             = "anthropic";
    static constexpr std::string_view default_base_url = "https://api.anthropic.com";

    static void configure_auth(httplib::Headers& hdrs, const LLMConfig& cfg) {
        hdrs.emplace("x-api-key", cfg.api_key);
        hdrs.emplace("anthropic-version",
            cfg.api_version.empty() ? "2023-06-01" : cfg.api_version);
    }

    static std::string request_path(std::string_view /*model*/, const LLMConfig& /*cfg*/) {
        return "/v1/messages";
    }

    // ── Serialization ───────────────────────────────────────────────────

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

    // ── Core trait interface ─────────────────────────────────────────────

    static json build_request(std::string_view model,
                              const std::vector<Message>& messages,
                              const std::vector<ToolSchema>& tools,
                              const LLMConfig& cfg) {
        json body;
        body["model"]       = model;
        body["max_tokens"]  = cfg.max_tokens;
        body["temperature"] = cfg.temperature;

        if (cfg.top_p)         body["top_p"] = *cfg.top_p;
        if (cfg.top_k)         body["top_k"] = static_cast<int>(*cfg.top_k);
        if (!cfg.stop.empty()) body["stop_sequences"] = cfg.stop;

        json msgs = json::array();
        for (auto& m : messages) {
            if (m.role == Role::system) { body["system"] = m.text(); continue; }
            msgs.push_back(message_to_json(m));
        }
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

    static LLMResponse parse_response(const json& j) {
        return {
            parse_message(j),
            j.value("usage", json::object()),
            j.value("stop_reason", std::string{}),
            j
        };
    }
};

static_assert(provider_defined<anthropic>);

} // namespace tiny_agent
