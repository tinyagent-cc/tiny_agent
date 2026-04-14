#pragma once
#include "../core/llm.hpp"

namespace tiny_agent {

// ── Provider tag ────────────────────────────────────────────────────────────

struct gemini {};

// ── Traits specialization ───────────────────────────────────────────────────

template<>
struct provider_traits<gemini> {
    static constexpr std::string_view name             = "gemini";
    static constexpr std::string_view default_base_url = "https://generativelanguage.googleapis.com";

    static void configure_auth(httplib::Headers& hdrs, const LLMConfig& cfg) {
        for (auto& [k, v] : cfg.headers) hdrs.emplace(k, v);
    }

    static std::string request_path(std::string_view model, const LLMConfig& cfg) {
        return "/v1beta/models/" + std::string(model)
             + ":generateContent?key=" + cfg.api_key;
    }

    // ── Schema conversion (Gemini wants UPPERCASE type names) ───────────

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

    // ── Serialization ───────────────────────────────────────────────────

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

    // ── Core trait interface ─────────────────────────────────────────────

    static json build_request(std::string_view /*model*/,
                              const std::vector<Message>& messages,
                              const std::vector<ToolSchema>& tools,
                              const LLMConfig& cfg) {
        json body;

        json gen;
        gen["temperature"]     = cfg.temperature;
        gen["maxOutputTokens"] = cfg.max_tokens;
        if (cfg.top_p) gen["topP"] = *cfg.top_p;
        if (cfg.top_k) gen["topK"] = static_cast<int>(*cfg.top_k);
        if (!cfg.stop.empty()) gen["stopSequences"] = cfg.stop;
        body["generationConfig"] = gen;

        json contents = json::array();
        for (auto& m : messages) {
            if (m.role == Role::system) {
                body["systemInstruction"] = {
                    {"parts", json::array({{{"text", m.text()}}})}};
                continue;
            }
            contents.push_back(message_to_json(m));
        }
        body["contents"] = contents;

        if (!tools.empty()) {
            json decls = json::array();
            for (auto& t : tools) decls.push_back(tool_schema_to_json(t));
            body["tools"] = json::array({{{"functionDeclarations", decls}}});
        }

        if (!cfg.extra.empty()) body.merge_patch(cfg.extra);
        return body;
    }

    static LLMResponse parse_response(const json& j) {
        if (!j.contains("candidates") || j["candidates"].empty())
            throw APIError(0, "Gemini returned no candidates: " + j.dump());

        auto& candidate = j["candidates"][0];
        return {
            parse_candidate(candidate),
            j.value("usageMetadata", json::object()),
            candidate.value("finishReason", std::string{}),
            j
        };
    }
};

static_assert(provider_defined<gemini>);

} // namespace tiny_agent
