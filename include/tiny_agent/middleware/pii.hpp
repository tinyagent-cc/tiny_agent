#pragma once
#include "../core/middleware.hpp"
#include <regex>

namespace tiny_agent::middleware {

// Detect and handle Personally Identifiable Information in messages.
// Inspired by LangChain's PIIMiddleware.
//
// Built-in PII types: email, credit_card, ip, phone, ssn.
// Custom detectors can be supplied as regex strings.
//
// Strategies:
//   "redact"  – replace with [REDACTED_<TYPE>]   (default)
//   "block"   – throw on detection
//   "mask"    – partially mask (show last 4 chars)

struct PIIConfig {
    std::string pii_type;
    std::string strategy       = "redact";
    std::string custom_pattern;        // overrides built-in pattern
    bool apply_to_input  = true;
    bool apply_to_output = false;
};

namespace detail {

inline const std::unordered_map<std::string, std::string>& builtin_patterns() {
    static const std::unordered_map<std::string, std::string> p{
        {"email",       R"([a-zA-Z0-9._%+\-]+@[a-zA-Z0-9.\-]+\.[a-zA-Z]{2,})"},
        {"credit_card", R"(\b\d{4}[\s\-]?\d{4}[\s\-]?\d{4}[\s\-]?\d{4}\b)"},
        {"ip",          R"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b)"},
        {"phone",       R"(\+?\d{1,3}[\s.\-]?\(?\d{3}\)?[\s.\-]?\d{3}[\s.\-]?\d{4})"},
        {"ssn",         R"(\b\d{3}\-\d{2}\-\d{4}\b)"},
    };
    return p;
}

}  // namespace detail

inline MiddlewareFn pii(PIIConfig cfg) {
    std::string pattern = cfg.custom_pattern;
    if (pattern.empty()) {
        auto& builtins = detail::builtin_patterns();
        auto it = builtins.find(cfg.pii_type);
        if (it == builtins.end())
            throw Error("Unknown PII type '" + cfg.pii_type +
                        "' and no custom_pattern supplied");
        pattern = it->second;
    }

    auto re = std::make_shared<std::regex>(pattern);
    std::string pii_upper = cfg.pii_type;
    for (auto& c : pii_upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    return [cfg, re, pii_upper](std::vector<Message>& msgs, Next next) -> LLMResponse {
        auto scrub = [&](const std::string& text) {
            if (cfg.strategy == "block") {
                if (std::regex_search(text, *re))
                    throw Error("PII detected (" + cfg.pii_type + ") — blocked");
                return text;
            }
            if (cfg.strategy == "mask") {
                std::string result;
                std::sregex_iterator it(text.begin(), text.end(), *re), end;
                std::size_t pos = 0;
                for (; it != end; ++it) {
                    auto& m = *it;
                    result += text.substr(pos, static_cast<std::size_t>(m.position()) - pos);
                    auto s = m.str();
                    if (s.size() <= 4)
                        result += std::string(s.size(), '*');
                    else
                        result += std::string(s.size() - 4, '*') +
                                  s.substr(s.size() - 4);
                    pos = static_cast<std::size_t>(m.position() + m.length());
                }
                result += text.substr(pos);
                return result;
            }
            return std::regex_replace(text, *re, "[REDACTED_" + pii_upper + "]");
        };

        if (cfg.apply_to_input)
            for (auto& m : msgs)
                if (m.role == Role::user)
                    if (auto* s = std::get_if<std::string>(&m.content))
                        *s = scrub(*s);

        auto resp = next(msgs);

        if (cfg.apply_to_output)
            if (auto* s = std::get_if<std::string>(&resp.message.content))
                *s = scrub(*s);

        return resp;
    };
}

} // namespace tiny_agent::middleware
