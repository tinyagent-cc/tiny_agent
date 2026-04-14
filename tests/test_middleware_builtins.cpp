#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/middleware.hpp>

using namespace tiny_agent;

static LLMResponse ok(const std::string& text = "ok") {
    return {Message::assistant(text), {}, "stop", {}};
}

static LLMResponse with_tool_calls(int n) {
    LLMResponse resp = ok();
    for (int i = 0; i < n; ++i)
        resp.message.tool_calls.push_back(
            {"tc_" + std::to_string(i), "tool_" + std::to_string(i), json::object()});
    return resp;
}

// ═══════════════════════════════════════════════════════════════════════════
// ModelCallLimit
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("model_call_limit: allows calls under limit") {
    auto mw = middleware::model_call_limit({.run_limit = 3});
    std::vector<Message> msgs = {Message::user("hi")};
    for (int i = 0; i < 3; ++i) {
        auto resp = mw(msgs, [](auto&) { return ok(); });
        CHECK(resp.finish_reason == "stop");
    }
}

TEST_CASE("model_call_limit: end behavior returns message") {
    auto mw = middleware::model_call_limit(
        {.run_limit = 1, .exit_behavior = "end"});
    std::vector<Message> msgs;

    auto r1 = mw(msgs, [](auto&) { return ok(); });
    CHECK(r1.finish_reason == "stop");

    auto r2 = mw(msgs, [](auto&) { return ok("should not reach"); });
    CHECK(r2.finish_reason == "model_call_limit");
    CHECK(r2.message.text().find("maximum") != std::string::npos);
}

TEST_CASE("model_call_limit: error behavior throws") {
    auto mw = middleware::model_call_limit(
        {.run_limit = 1, .exit_behavior = "error"});
    std::vector<Message> msgs;

    mw(msgs, [](auto&) { return ok(); });
    CHECK_THROWS_AS(mw(msgs, [](auto&) { return ok(); }), Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// ToolCallLimit
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("tool_call_limit: allows under limit") {
    auto mw = middleware::tool_call_limit({.run_limit = 5});
    std::vector<Message> msgs;
    auto resp = mw(msgs, [](auto&) { return with_tool_calls(3); });
    CHECK(resp.message.tool_calls.size() == 3);
}

TEST_CASE("tool_call_limit: end clears tool calls") {
    auto mw = middleware::tool_call_limit(
        {.run_limit = 2, .exit_behavior = "end"});
    std::vector<Message> msgs;

    auto r1 = mw(msgs, [](auto&) { return with_tool_calls(2); });
    CHECK(r1.message.tool_calls.size() == 2);

    auto r2 = mw(msgs, [](auto&) { return with_tool_calls(3); });
    CHECK(r2.message.tool_calls.empty());
    CHECK(r2.finish_reason == "tool_call_limit");
}

TEST_CASE("tool_call_limit: error throws") {
    auto mw = middleware::tool_call_limit(
        {.run_limit = 1, .exit_behavior = "error"});
    std::vector<Message> msgs;

    mw(msgs, [](auto&) { return with_tool_calls(1); });
    CHECK_THROWS_AS(
        mw(msgs, [](auto&) { return with_tool_calls(1); }), Error);
}

TEST_CASE("tool_call_limit: per-tool filtering") {
    auto mw = middleware::tool_call_limit(
        {.run_limit = 2, .tool_name = "search", .exit_behavior = "end"});
    std::vector<Message> msgs;

    // Create a response with mixed tool calls
    auto make_mixed = []() {
        LLMResponse resp = ok();
        resp.message.tool_calls = {
            {"tc_1", "search", {}},
            {"tc_2", "calc",   {}},
            {"tc_3", "search", {}},
        };
        return resp;
    };

    auto r1 = mw(msgs, [&](auto&) { return make_mixed(); });
    CHECK(r1.message.tool_calls.size() == 3);

    // Second call pushes search count to 4 > limit 2 → cleared
    auto r2 = mw(msgs, [&](auto&) { return make_mixed(); });
    CHECK(r2.message.tool_calls.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// ModelRetry
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("model_retry: succeeds on first try") {
    auto mw = middleware::model_retry({.max_retries = 2});
    std::vector<Message> msgs;
    auto resp = mw(msgs, [](auto&) { return ok("first"); });
    CHECK(resp.message.text() == "first");
}

TEST_CASE("model_retry: retries then succeeds") {
    auto mw = middleware::model_retry(
        {.max_retries = 3, .initial_delay = 1.0, .jitter = false});
    std::vector<Message> msgs;

    int calls = 0;
    auto resp = mw(msgs, [&](auto&) -> LLMResponse {
        if (++calls < 3) throw APIError(500, "server error");
        return ok("recovered");
    });
    CHECK(resp.message.text() == "recovered");
    CHECK(calls == 3);
}

TEST_CASE("model_retry: continue mode returns error message") {
    auto mw = middleware::model_retry(
        {.max_retries = 1, .initial_delay = 1.0,
         .jitter = false, .on_failure = "continue"});
    std::vector<Message> msgs;

    auto resp = mw(msgs, [](auto&) -> LLMResponse {
        throw APIError(500, "boom");
    });
    CHECK(resp.finish_reason == "error");
    CHECK(resp.message.text().find("boom") != std::string::npos);
}

TEST_CASE("model_retry: error mode rethrows") {
    auto mw = middleware::model_retry(
        {.max_retries = 0, .on_failure = "error"});
    std::vector<Message> msgs;
    CHECK_THROWS_AS(
        mw(msgs, [](auto&) -> LLMResponse { throw APIError(500, "fail"); }),
        APIError);
}

// ═══════════════════════════════════════════════════════════════════════════
// PII
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("pii: redact email") {
    auto mw = middleware::pii({.pii_type = "email", .strategy = "redact"});
    std::vector<Message> msgs = {Message::user("Contact me at alice@example.com please")};

    mw(msgs, [&](auto& m) {
        CHECK(m[0].text().find("alice@example.com") == std::string::npos);
        CHECK(m[0].text().find("[REDACTED_EMAIL]") != std::string::npos);
        return ok();
    });
}

TEST_CASE("pii: block throws on detection") {
    auto mw = middleware::pii({.pii_type = "ssn", .strategy = "block"});
    std::vector<Message> msgs = {Message::user("My SSN is 123-45-6789")};
    CHECK_THROWS_AS(mw(msgs, [](auto&) { return ok(); }), Error);
}

TEST_CASE("pii: mask preserves last 4 chars") {
    auto mw = middleware::pii({.pii_type = "email", .strategy = "mask"});
    std::vector<Message> msgs = {Message::user("Email: test@example.com")};

    mw(msgs, [&](auto& m) {
        auto text = m[0].text();
        CHECK(text.find("test@example.com") == std::string::npos);
        CHECK(text.find(".com") != std::string::npos);
        return ok();
    });
}

TEST_CASE("pii: custom pattern") {
    auto mw = middleware::pii({
        .pii_type = "api_key",
        .strategy = "redact",
        .custom_pattern = R"(sk-[a-zA-Z0-9]{8})"});
    std::vector<Message> msgs = {Message::user("Key: sk-abc12345")};

    mw(msgs, [&](auto& m) {
        CHECK(m[0].text().find("sk-abc12345") == std::string::npos);
        CHECK(m[0].text().find("[REDACTED_API_KEY]") != std::string::npos);
        return ok();
    });
}

TEST_CASE("pii: no PII passes through unchanged") {
    auto mw = middleware::pii({.pii_type = "credit_card"});
    std::vector<Message> msgs = {Message::user("Just a normal message")};

    mw(msgs, [&](auto& m) {
        CHECK(m[0].text() == "Just a normal message");
        return ok();
    });
}

TEST_CASE("pii: apply_to_output") {
    auto mw = middleware::pii({
        .pii_type = "email",
        .strategy = "redact",
        .apply_to_input = false,
        .apply_to_output = true});
    std::vector<Message> msgs = {Message::user("hi")};

    auto resp = mw(msgs, [](auto&) { return ok("Reply to alice@example.com"); });
    CHECK(resp.message.text().find("alice@example.com") == std::string::npos);
    CHECK(resp.message.text().find("[REDACTED_EMAIL]") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// ContextEditing
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("context_editing: no-op when under trigger") {
    auto mw = middleware::context_editing({.trigger = 100'000});
    std::vector<Message> msgs = {
        Message::user("hi"),
        Message::tool_result("tc1", "short result"),
    };
    auto orig_size = msgs.size();
    mw(msgs, [&](auto& m) {
        CHECK(m.size() == orig_size);
        return ok();
    });
}

TEST_CASE("context_editing: clears old tool results") {
    auto mw = middleware::context_editing({.trigger = 1, .keep = 1});

    // Build messages with enough "tokens" to trigger (trigger=1 means always)
    std::vector<Message> msgs = {
        Message::user("hello world"),
        Message::tool_result("tc1", "first result data data data"),
        Message::tool_result("tc2", "second result data data data"),
        Message::tool_result("tc3", "third result kept"),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m[1].text() == "[cleared]");
        CHECK(m[2].text() == "[cleared]");
        CHECK(m[3].text() == "third result kept");
        return ok();
    });
}

TEST_CASE("context_editing: custom placeholder") {
    auto mw = middleware::context_editing(
        {.trigger = 1, .keep = 0, .placeholder = "<removed>"});
    std::vector<Message> msgs = {
        Message::user("x"),
        Message::tool_result("tc1", "data"),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m[1].text() == "<removed>");
        return ok();
    });
}
