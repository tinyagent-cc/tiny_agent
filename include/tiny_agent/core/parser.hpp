#pragma once
#include "types.hpp"
#include <concepts>

namespace tiny_agent {

template<typename P>
concept output_parser = requires(const LLMResponse& resp) {
    typename P::output_type;
    { P::parse(resp) } -> std::convertible_to<typename P::output_type>;
};

struct TextParser {
    using output_type = std::string;
    static std::string parse(const LLMResponse& resp) {
        return resp.message.text();
    }
};

static_assert(output_parser<TextParser>);

template<typename T>
struct JsonParser {
    using output_type = T;

    static T parse(const LLMResponse& resp) {
        auto text = resp.message.text();
        try {
            return json::parse(text).template get<T>();
        } catch (const json::exception& e) {
            throw ParseError(
                std::string("JsonParser: failed to parse LLM response: ") + e.what());
        }
    }
};

struct JsonValueParser {
    using output_type = json;

    static json parse(const LLMResponse& resp) {
        auto text = resp.message.text();
        try {
            return json::parse(text);
        } catch (const json::exception& e) {
            throw ParseError(
                std::string("JsonValueParser: failed to parse: ") + e.what());
        }
    }
};

static_assert(output_parser<JsonValueParser>);

template<typename Inner = TextParser>
    requires output_parser<Inner>
struct StripMarkdownParser {
    using output_type = typename Inner::output_type;

    static output_type parse(const LLMResponse& resp) {
        auto text = resp.message.text();

        if (auto start = text.find("```"); start != std::string::npos) {
            auto content_start = text.find('\n', start);
            if (content_start != std::string::npos) {
                auto end = text.find("```", content_start);
                if (end != std::string::npos)
                    text = text.substr(content_start + 1, end - content_start - 1);
            }
        }

        LLMResponse modified = resp;
        modified.message.content = std::move(text);
        return Inner::parse(modified);
    }
};

template<output_parser Inner, typename Pred>
    requires std::predicate<Pred, const typename Inner::output_type&>
struct ValidatingParser {
    using output_type = typename Inner::output_type;

    static output_type parse(const LLMResponse& resp) {
        auto result = Inner::parse(resp);
        if (!Pred{}(result))
            throw ValidationError("ValidatingParser: validation failed");
        return result;
    }
};

} // namespace tiny_agent
