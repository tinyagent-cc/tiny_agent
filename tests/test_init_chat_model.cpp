#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/init_chat_model.hpp>

using namespace tiny_agent;

// ── parse_model_string ──────────────────────────────────────────────────────

TEST_CASE("parse explicit provider:model") {
    auto s = parse_model_string("openai:gpt-4o-mini");
    CHECK(s.provider == "openai");
    CHECK(s.model    == "gpt-4o-mini");
}

TEST_CASE("parse explicit anthropic") {
    auto s = parse_model_string("anthropic:claude-sonnet-4-20250514");
    CHECK(s.provider == "anthropic");
    CHECK(s.model    == "claude-sonnet-4-20250514");
}

TEST_CASE("parse explicit gemini") {
    auto s = parse_model_string("gemini:gemini-2.0-flash");
    CHECK(s.provider == "gemini");
    CHECK(s.model    == "gemini-2.0-flash");
}

TEST_CASE("auto-detect openai from model prefix") {
    CHECK(parse_model_string("gpt-4o").provider      == "openai");
    CHECK(parse_model_string("gpt-4o-mini").provider  == "openai");
    CHECK(parse_model_string("o1-preview").provider   == "openai");
    CHECK(parse_model_string("o3-mini").provider      == "openai");
    CHECK(parse_model_string("chatgpt-4o").provider   == "openai");
}

TEST_CASE("auto-detect anthropic from model prefix") {
    CHECK(parse_model_string("claude-sonnet-4-20250514").provider   == "anthropic");
    CHECK(parse_model_string("claude-3-haiku-20240307").provider == "anthropic");
}

TEST_CASE("auto-detect gemini from model prefix") {
    CHECK(parse_model_string("gemini-2.0-flash").provider  == "gemini");
    CHECK(parse_model_string("gemini-1.5-pro").provider    == "gemini");
}

TEST_CASE("unknown model defaults to openai") {
    auto s = parse_model_string("my-custom-model");
    CHECK(s.provider == "openai");
    CHECK(s.model    == "my-custom-model");
}

TEST_CASE("edge: empty string") {
    auto s = parse_model_string("");
    CHECK(s.provider == "openai");
    CHECK(s.model.empty());
}

TEST_CASE("edge: colon-only is treated as non-provider") {
    auto s = parse_model_string(":");
    CHECK(s.provider == "openai");
    CHECK(s.model    == ":");
}

// ── init_chat_model construction (offline — just verifies it compiles) ──────

TEST_CASE("init_chat_model returns AnyLLM (openai)") {
    // We can't actually call chat() without a valid key, but we can verify
    // the factory selects the right provider by checking model_name().
    auto llm = init_chat_model("openai:gpt-4o-mini",
                               LLMConfig{.api_key = "fake-key"});
    CHECK(llm.model_name() == "gpt-4o-mini");
}

TEST_CASE("init_chat_model returns AnyLLM (anthropic)") {
    auto llm = init_chat_model("anthropic:claude-sonnet-4-20250514",
                               LLMConfig{.api_key = "fake-key"});
    CHECK(llm.model_name() == "claude-sonnet-4-20250514");
}

TEST_CASE("init_chat_model returns AnyLLM (gemini)") {
    auto llm = init_chat_model("gemini:gemini-2.0-flash",
                               LLMConfig{.api_key = "fake-key"});
    CHECK(llm.model_name() == "gemini-2.0-flash");
}

TEST_CASE("init_chat_model auto-detect") {
    auto llm = init_chat_model("gpt-4o", LLMConfig{.api_key = "fake"});
    CHECK(llm.model_name() == "gpt-4o");
}

TEST_CASE("init_chat_model unknown provider throws") {
    CHECK_THROWS_AS(
        init_chat_model("unknownprovider:model", LLMConfig{.api_key = "x"}),
        Error);
}

TEST_CASE("init_chat_model two-arg overload") {
    auto llm = init_chat_model("openai", "gpt-4o-mini",
                               LLMConfig{.api_key = "fake"});
    CHECK(llm.model_name() == "gpt-4o-mini");
}
