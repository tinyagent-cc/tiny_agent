#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <functional>
#include <stdexcept>
#include <concepts>

namespace tiny_agent {

using json = nlohmann::json;

// ── Errors ──────────────────────────────────────────────────────────────────

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

struct APIError : Error {
    int status_code;
    APIError(int code, std::string msg)
        : Error(std::move(msg)), status_code(code) {}
};

struct ToolError       : Error { using Error::Error; };
struct MCPError        : Error { using Error::Error; };
struct ParseError      : Error { using Error::Error; };
struct ValidationError : Error { using Error::Error; };

// ── Role ────────────────────────────────────────────────────────────────────

enum class Role { system, user, assistant, tool };

constexpr const char* to_string(Role r) noexcept {
    switch (r) {
        case Role::system:    return "system";
        case Role::user:      return "user";
        case Role::assistant: return "assistant";
        case Role::tool:      return "tool";
    }
    return "user";
}

constexpr Role role_from_string(std::string_view s) noexcept {
    if (s == "system")    return Role::system;
    if (s == "assistant") return Role::assistant;
    if (s == "tool")      return Role::tool;
    return Role::user;
}

// ── Content (multimodal) ────────────────────────────────────────────────────

struct ContentPart {
    std::string type;   // "text" | "image_url"
    std::string text;

    struct ImageURL { std::string url; std::string detail = "auto"; };
    std::optional<ImageURL> image_url;
};

// ── Tool calls ──────────────────────────────────────────────────────────────

struct ToolCall {
    std::string id;
    std::string name;
    json        arguments;
};

// ── Message ─────────────────────────────────────────────────────────────────

struct Message {
    Role role;
    std::variant<std::string, std::vector<ContentPart>> content;
    std::vector<ToolCall>          tool_calls;
    std::optional<std::string>     tool_call_id;
    std::optional<std::string>     name;

    static Message system(std::string t)    { return {Role::system,    std::move(t), {}, {}, {}}; }
    static Message user(std::string t)      { return {Role::user,      std::move(t), {}, {}, {}}; }
    static Message assistant(std::string t) { return {Role::assistant,  std::move(t), {}, {}, {}}; }

    static Message tool_result(std::string id, std::string body) {
        return {Role::tool, std::move(body), {}, std::move(id), {}};
    }

    static Message image(std::string text, std::string url, std::string detail = "auto") {
        std::vector<ContentPart> parts;
        if (!text.empty())
            parts.push_back({"text", std::move(text), {}});
        parts.push_back({"image_url", {}, ContentPart::ImageURL{std::move(url), std::move(detail)}});
        return {Role::user, std::move(parts), {}, {}, {}};
    }

    [[nodiscard]] std::string text() const {
        if (auto* s = std::get_if<std::string>(&content)) return *s;
        if (auto* v = std::get_if<std::vector<ContentPart>>(&content))
            for (auto& p : *v)
                if (p.type == "text") return p.text;
        return {};
    }

    [[nodiscard]] bool has_tool_calls() const { return !tool_calls.empty(); }
};

// ── LLM response ───────────────────────────────────────────────────────────

struct LLMResponse {
    Message     message;
    json        usage;
    std::string finish_reason;
    json        raw;

    template<typename F>
    auto map(F&& f) const -> LLMResponse {
        auto mapped = Message::assistant(f(message.text()));
        return {std::move(mapped), usage, finish_reason, raw};
    }
};

} // namespace tiny_agent
