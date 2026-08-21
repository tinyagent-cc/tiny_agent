#pragma once
// Offline stand-ins for a chat model and an embeddings model. Every test that
// exercises the agent loop, the middleware chain or a retriever without a
// network uses these, so a scripted turn sequence lives in one place.

#include <tiny_agent/core/model.hpp>
#include <tiny_agent/core/types.hpp>
#include <functional>
#include <string>
#include <vector>

namespace tiny_agent::test {

// A chat model that replays a scripted list of responses, one per call, and
// repeats the last one once the script runs out. It records every message
// vector it was handed so a test can assert on what the agent actually sent.
struct MockChat {
    using input_t   = std::string;
    using output_t  = std::string;
    using model_tag = chat_tag;

    std::vector<LLMResponse>          script;
    std::vector<std::vector<Message>> seen;
    std::size_t                       calls = 0;
    std::function<void()>             on_call;

    static LLMResponse text(std::string t, std::string finish = "stop") {
        return {Message::assistant(std::move(t)), {}, std::move(finish), {}};
    }

    static LLMResponse tool_call(std::string name, json args = json::object(),
                                 std::string id = "call_1") {
        LLMResponse r = text("", "tool_calls");
        r.message.tool_calls.push_back({std::move(id), std::move(name), std::move(args)});
        return r;
    }

    LLMResponse chat(const std::vector<Message>& msgs,
                     const std::vector<ToolSchema>& = {},
                     const LLMConfig& = {}) {
        seen.push_back(msgs);
        if (on_call) on_call();
        if (script.empty()) return text("ok");
        auto i = calls < script.size() ? calls : script.size() - 1;
        ++calls;
        return script[i];
    }

    std::string invoke(std::string prompt, const RunConfig& = {}) {
        std::vector<Message> msgs = {Message::user(std::move(prompt))};
        return chat(msgs).message.text();
    }

    std::string model_name() const { return "mock-chat"; }
    float get_temperature() const { return 0.0f; }

    std::vector<std::string> batch(std::vector<std::string> prompts, const RunConfig& cfg = {}) {
        std::vector<std::string> out;
        out.reserve(prompts.size());
        for (auto& p : prompts) out.push_back(invoke(std::move(p), cfg));
        return out;
    }

    void stream(std::string prompt, std::function<void(std::string)> cb, const RunConfig& cfg = {}) {
        cb(invoke(std::move(prompt), cfg));
    }
};

static_assert(is_chat<MockChat>);

// Deterministic embeddings: three dimensions derived from the text, so the same
// string always maps to the same vector and similarity is reproducible.
struct MockEmbed {
    using input_t   = std::string;
    using output_t  = std::vector<float>;
    using model_tag = embedding_tag;

    std::size_t dims = 3;
    // Set to a non-zero value to return that many vectors regardless of how many
    // texts were asked for, which is how a real provider misbehaves under a
    // partial failure.
    std::size_t force_count = 0;

    std::vector<float> invoke(const std::string& t, const RunConfig& = {}) { return embed_query(t); }

    std::vector<float> embed_query(const std::string& text) const {
        std::vector<float> v(dims, 0.0f);
        for (std::size_t i = 0; i < text.size(); ++i)
            v[i % dims] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0f;
        return v;
    }

    std::vector<std::vector<float>> embed_documents(const std::vector<std::string>& texts) const {
        std::vector<std::vector<float>> out;
        auto n = force_count ? force_count : texts.size();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(embed_query(i < texts.size() ? texts[i] : std::string{}));
        return out;
    }

    std::string model_name() const { return "mock-embed"; }
    std::size_t dimensions() const { return dims; }

    std::vector<std::vector<float>> batch(std::vector<std::string> texts, const RunConfig& = {}) {
        return embed_documents(texts);
    }
    void stream(std::string text, std::function<void(std::vector<float>)> cb, const RunConfig& = {}) {
        cb(embed_query(text));
    }
};

static_assert(is_embedding<MockEmbed>);

} // namespace tiny_agent::test
