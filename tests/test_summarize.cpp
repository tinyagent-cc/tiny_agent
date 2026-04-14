#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/middleware.hpp>
#include <tiny_agent/middleware/summarize.hpp>

using namespace tiny_agent;

static LLMResponse ok(const std::string& text = "ok") {
    return {Message::assistant(text), {}, "stop", {}};
}

// ═══════════════════════════════════════════════════════════════════════════
// extractive_summarize
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("extractive_summarize: produces role-prefixed output") {
    std::vector<Message> msgs = {
        Message::user("What is 2+2?"),
        Message::assistant("4"),
    };
    auto summary = middleware::extractive_summarize(msgs);
    CHECK(summary.find("user:") != std::string::npos);
    CHECK(summary.find("assistant:") != std::string::npos);
    CHECK(summary.find("2+2") != std::string::npos);
}

TEST_CASE("extractive_summarize: truncates long messages") {
    std::vector<Message> msgs = {
        Message::user(std::string(500, 'x')),
    };
    auto summary = middleware::extractive_summarize(msgs, 50);
    CHECK(summary.find("...") != std::string::npos);
    CHECK(summary.size() < 500);
}

TEST_CASE("extractive_summarize: tool results truncated more aggressively") {
    auto tool_msg = Message::tool_result("tc1", std::string(300, 'd'));
    std::vector<Message> msgs = {tool_msg};
    auto summary = middleware::extractive_summarize(msgs, 150);
    CHECK(summary.find("...") != std::string::npos);
    CHECK(summary.size() < 200);
}

TEST_CASE("extractive_summarize: includes tool name when present") {
    auto tool_msg = Message::tool_result("tc1", "result");
    tool_msg.name = "search";
    std::vector<Message> msgs = {tool_msg};
    auto summary = middleware::extractive_summarize(msgs);
    CHECK(summary.find("(search)") != std::string::npos);
}

TEST_CASE("extractive_summarize: skips empty messages") {
    std::vector<Message> msgs = {
        Message::user(""),
        Message::assistant("hello"),
    };
    auto summary = middleware::extractive_summarize(msgs);
    CHECK(summary.find("user:") == std::string::npos);
    CHECK(summary.find("assistant:") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// Summarize<> template middleware
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Summarize: no-op when under token threshold") {
    middleware::Summarize<10000, 2> mw;
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::user("hi"),
        Message::assistant("hello"),
    };
    auto orig_size = msgs.size();
    mw(msgs, [&](auto& m) {
        CHECK(m.size() == orig_size);
        return ok();
    });
}

TEST_CASE("Summarize: compresses when over threshold") {
    middleware::Summarize<1, 2> mw;  // trigger=1 → always compress
    std::vector<Message> msgs = {
        Message::system("system prompt"),
        Message::user("first question"),
        Message::assistant("first answer"),
        Message::user("second question"),
        Message::assistant("second answer"),
        Message::user("third question"),
        Message::assistant("third answer"),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m.size() == 4);  // system + summary + 2 recent
        CHECK(m[0].role == Role::system);
        CHECK(m[0].text() == "system prompt");
        CHECK(m[1].role == Role::system);
        CHECK(m[1].text().find("[Conversation summary]") != std::string::npos);
        CHECK(m[1].text().find("first question") != std::string::npos);
        return ok();
    });
}

TEST_CASE("Summarize: preserves recent messages") {
    middleware::Summarize<1, 3> mw;
    std::vector<Message> msgs = {
        Message::user("old1"),
        Message::assistant("old2"),
        Message::user("keep1"),
        Message::assistant("keep2"),
        Message::user("keep3"),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m.back().text() == "keep3");
        CHECK(m[m.size()-2].text() == "keep2");
        CHECK(m[m.size()-3].text() == "keep1");
        return ok();
    });
}

TEST_CASE("Summarize: works without system prompt") {
    middleware::Summarize<1, 2> mw;
    std::vector<Message> msgs = {
        Message::user("What is the capital of France and why is it important?"),
        Message::assistant("Paris is the capital and a major cultural center."),
        Message::user("Tell me about the history of that city in detail."),
        Message::assistant("Paris has a rich history spanning many centuries."),
        Message::user("What about the modern economy there these days?"),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m.front().role == Role::system);
        CHECK(m.front().text().find("[Conversation summary]") != std::string::npos);
        CHECK(m.back().text().find("modern economy") != std::string::npos);
        return ok();
    });
}

TEST_CASE("Summarize: custom summarizer function") {
    middleware::Summarize<1, 2> mw{
        [](const std::vector<Message>&) { return "custom summary"; }
    };
    std::vector<Message> msgs = {
        Message::system("You are a helpful assistant for the user."),
        Message::user("What is the capital of France and why is it important?"),
        Message::assistant("Paris is the capital and a major cultural center."),
        Message::user("Tell me about the history of that city in detail."),
        Message::assistant("Paris has a rich history spanning many centuries."),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m[1].text().find("custom summary") != std::string::npos);
        return ok();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// summarize() runtime factory
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("summarize runtime: no-op under threshold") {
    auto mw = middleware::summarize({.trigger_tokens = 100000});
    std::vector<Message> msgs = {Message::user("hi")};
    auto orig_size = msgs.size();
    mw(msgs, [&](auto& m) {
        CHECK(m.size() == orig_size);
        return ok();
    });
}

TEST_CASE("summarize runtime: compresses with default summarizer") {
    auto mw = middleware::summarize({.trigger_tokens = 1, .keep_recent = 2});
    std::vector<Message> msgs = {
        Message::system("You are a helpful assistant for the user."),
        Message::user("What is the capital of France and why is it important?"),
        Message::assistant("Paris is the capital and a major cultural center."),
        Message::user("Tell me about the history of that city in detail."),
        Message::assistant("Paris has a rich history spanning many centuries."),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m.size() == 4);
        CHECK(m[1].text().find("[Conversation summary]") != std::string::npos);
        return ok();
    });
}

TEST_CASE("summarize runtime: custom summarizer") {
    auto mw = middleware::summarize({
        .trigger_tokens = 1,
        .keep_recent = 1,
        .summarizer = [](const std::vector<Message>&) {
            return "runtime custom summary";
        },
    });
    std::vector<Message> msgs = {
        Message::user("This is a fairly long older message that should be summarized."),
        Message::assistant("This is the older response that should also be summarized."),
        Message::user("This is the recent message that should be kept intact."),
    };

    mw(msgs, [&](auto& m) {
        CHECK(m[0].text().find("runtime custom summary") != std::string::npos);
        return ok();
    });
}

// ═══════════════════════════════════════════════════════════════════════════
// Chain integration
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Summarize works in MiddlewareChain") {
    MiddlewareChain chain;
    chain.add(middleware::summarize({.trigger_tokens = 1, .keep_recent = 2}));

    std::vector<Message> msgs = {
        Message::system("You are a helpful assistant for the user."),
        Message::user("What is the capital of France and why?"),
        Message::assistant("Paris is the capital of France."),
        Message::user("Tell me about the history of that city."),
        Message::assistant("Paris has a rich history spanning centuries."),
    };

    auto resp = chain.run(msgs, [](auto& m) {
        return ok(std::to_string(m.size()));
    });
    CHECK(resp.message.text() == "4");
}

TEST_CASE("Summarize works in StaticMiddlewareStack") {
    middleware::Summarize<1, 2> mw;
    auto stack = StaticMiddlewareStack{mw};

    std::vector<Message> msgs = {
        Message::system("You are a helpful assistant for the user."),
        Message::user("What is the capital of France and why?"),
        Message::assistant("Paris is the capital of France."),
        Message::user("Tell me about the history of that city."),
        Message::assistant("Paris has a rich history spanning centuries."),
    };

    auto resp = stack.run(msgs, [](auto& m) {
        return ok(std::to_string(m.size()));
    });
    CHECK(resp.message.text() == "4");
}
