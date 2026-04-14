#pragma once
#include "../core/llm.hpp"

namespace tiny_agent {

// ── Provider tag ────────────────────────────────────────────────────────────

struct openai {};

// ── Traits specialization ───────────────────────────────────────────────────

template<>
struct provider_traits<openai> {
    static constexpr std::string_view name         = "openai";
    static constexpr std::string_view default_base_url = "https://api.openai.com";

    static void configure_auth(httplib::Headers& hdrs, const LLMConfig& cfg) {
        if (!cfg.api_key.empty())
            hdrs.emplace("Authorization", "Bearer " + cfg.api_key);
    }

    static std::string request_path(std::string_view /*model*/, const LLMConfig& cfg) {
        std::string path = "/v1/chat/completions";
        if (!cfg.api_version.empty())
            path += "?api-version=" + cfg.api_version;
        return path;
    }

    // ── Serialization ───────────────────────────────────────────────────

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

    // ── Core trait interface ─────────────────────────────────────────────

    static json build_request(std::string_view model,
                              const std::vector<Message>& messages,
                              const std::vector<ToolSchema>& tools,
                              const LLMConfig& cfg) {
        json body;
        body["model"]       = model;
        body["temperature"] = cfg.temperature;
        body["max_tokens"]  = cfg.max_tokens;

        if (cfg.top_p)              body["top_p"]              = *cfg.top_p;
        if (cfg.frequency_penalty)  body["frequency_penalty"]  = *cfg.frequency_penalty;
        if (cfg.presence_penalty)   body["presence_penalty"]   = *cfg.presence_penalty;
        if (cfg.seed)               body["seed"]               = *cfg.seed;
        if (!cfg.stop.empty())      body["stop"]               = cfg.stop;
        if (cfg.response_format) {
            body["response_format"] = {{"type", *cfg.response_format}};
        }

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

        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
    }

    static LLMResponse parse_response(const json& j) {
        auto& choice = j["choices"][0];
        return {
            parse_choice(choice),
            j.value("usage", json::object()),
            choice.value("finish_reason", std::string{}),
            j
        };
    }
};

static_assert(provider_defined<openai>);

} // namespace tiny_agent
