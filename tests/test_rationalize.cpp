#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/middleware.hpp>

using namespace tiny_agent;

static LLMResponse ok(const std::string& text = "ok") {
    return {Message::assistant(text), {}, "stop", {}};
}

static std::string big_tool_result(std::size_t approx_tokens) {
    return std::string(approx_tokens * 4, 'x');
}

// ═══════════════════════════════════════════════════════════════════════════
// Rationalize<> template middleware
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Rationalize: no-op when no large content") {
    middleware::Rationalize<2000> mw;
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::user("hi"),
        Message::tool_result("tc1", "short result"),
    };

    std::string sys_before = msgs.front().text();
    mw(msgs, [&](auto& m) {
        CHECK(m.front().text() == sys_before);
        return ok();
    });
}

TEST_CASE("Rationalize: injects guidance for large tool results") {
    middleware::Rationalize<100> mw;  // threshold=100 tokens
    std::vector<Message> msgs = {
        Message::system("Be helpful."),
        Message::user("process this"),
        Message::tool_result("tc1", big_tool_result(500)),
    };

    mw(msgs, [&](auto& m) {
        auto sys_text = m.front().text();
        CHECK(sys_text.find("[Efficiency guidance") != std::string::npos);
        CHECK(sys_text.find("sed") != std::string::npos);
        CHECK(sys_text.find("Be helpful.") != std::string::npos);
        return ok();
    });
}

TEST_CASE("Rationalize: restores original system prompt after call") {
    middleware::Rationalize<100> mw;
    std::vector<Message> msgs = {
        Message::system("original prompt"),
        Message::tool_result("tc1", big_tool_result(500)),
    };

    mw(msgs, [](auto&) { return ok(); });
    CHECK(msgs.front().text() == "original prompt");
}

TEST_CASE("Rationalize: inserts temporary system msg when none exists") {
    middleware::Rationalize<100> mw;
    std::vector<Message> msgs = {
        Message::user("hi"),
        Message::tool_result("tc1", big_tool_result(500)),
    };

    auto orig_size = msgs.size();
    mw(msgs, [&](auto& m) {
        CHECK(m.size() == orig_size + 1);
        CHECK(m.front().role == Role::system);
        CHECK(m.front().text().find("[Efficiency guidance") != std::string::npos);
        return ok();
    });
    CHECK(msgs.size() == orig_size);  // removed after call
}

TEST_CASE("Rationalize: counts multiple large results") {
    middleware::Rationalize<100> mw;
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", big_tool_result(200)),
        Message::tool_result("tc2", big_tool_result(300)),
        Message::tool_result("tc3", "small"),
    };

    mw(msgs, [&](auto& m) {
        auto sys_text = m.front().text();
        CHECK(sys_text.find("2 large result(s)") != std::string::npos);
        return ok();
    });
}

TEST_CASE("Rationalize: custom hints") {
    middleware::Rationalize<100> mw{
        {"Use batch API calls instead of sequential ones"}
    };
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", big_tool_result(500)),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m.front().text().find("batch API") != std::string::npos);
        CHECK(m.front().text().find("sed") == std::string::npos);
        return ok();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// rationalize() runtime factory
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("rationalize runtime: no-op with small content") {
    auto mw = middleware::rationalize({.large_threshold = 10000});
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", "short"),
    };

    std::string sys_before = msgs.front().text();
    mw(msgs, [&](auto& m) {
        CHECK(m.front().text() == sys_before);
        return ok();
    });
}

TEST_CASE("rationalize runtime: injects default hints") {
    auto mw = middleware::rationalize({.large_threshold = 50});
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", big_tool_result(200)),
    };

    mw(msgs, [&](auto& m) {
        auto sys = m.front().text();
        CHECK(sys.find("[Efficiency guidance") != std::string::npos);
        CHECK(sys.find("sed") != std::string::npos);
        return ok();
    });
    CHECK(msgs.front().text() == "sys");
}

TEST_CASE("rationalize runtime: custom hints") {
    auto mw = middleware::rationalize({
        .large_threshold = 50,
        .hints = {"Use streaming for large payloads"},
    });
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", big_tool_result(200)),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m.front().text().find("streaming") != std::string::npos);
        return ok();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// Chain integration
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Rationalize works in MiddlewareChain") {
    MiddlewareChain chain;
    chain.add(middleware::rationalize({.large_threshold = 50}));

    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", big_tool_result(200)),
    };

    auto resp = chain.run(msgs, [](auto& m) {
        auto sys = m.front().text();
        bool has_guidance = sys.find("[Efficiency guidance") != std::string::npos;
        return ok(has_guidance ? "guided" : "plain");
    });
    CHECK(resp.message.text() == "guided");
    CHECK(msgs.front().text() == "sys");
}

TEST_CASE("Rationalize works in StaticMiddlewareStack") {
    middleware::Rationalize<50> mw;
    auto stack = StaticMiddlewareStack{mw};

    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::tool_result("tc1", big_tool_result(200)),
    };

    auto resp = stack.run(msgs, [](auto& m) {
        auto sys = m.front().text();
        bool has_guidance = sys.find("[Efficiency guidance") != std::string::npos;
        return ok(has_guidance ? "guided" : "plain");
    });
    CHECK(resp.message.text() == "guided");
    CHECK(msgs.front().text() == "sys");
}
