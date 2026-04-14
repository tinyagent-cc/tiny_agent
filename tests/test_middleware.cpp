#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/middleware.hpp>

using namespace tiny_agent;

static LLMResponse make_response(const std::string& text) {
    return {Message::assistant(text), {}, "stop", {}};
}

TEST_CASE("Empty middleware chain passes through") {
    MiddlewareChain chain;
    std::vector<Message> msgs = {Message::user("hello")};
    auto resp = chain.run(msgs, [](auto&) { return make_response("world"); });
    CHECK(resp.message.text() == "world");
}

TEST_CASE("Middleware can modify messages") {
    MiddlewareChain chain;
    chain.add([](std::vector<Message>& msgs, Next next) -> LLMResponse {
        msgs.push_back(Message::system("injected"));
        return next(msgs);
    });

    std::vector<Message> msgs = {Message::user("hi")};
    auto resp = chain.run(msgs, [](auto& m) {
        return make_response("got " + std::to_string(m.size()) + " msgs");
    });
    CHECK(resp.message.text() == "got 2 msgs");
}

TEST_CASE("Middlewares execute in order") {
    MiddlewareChain chain;
    std::string order;

    chain.add([&](std::vector<Message>& msgs, Next next) -> LLMResponse {
        order += "A";
        auto r = next(msgs);
        order += "a";
        return r;
    });
    chain.add([&](std::vector<Message>& msgs, Next next) -> LLMResponse {
        order += "B";
        auto r = next(msgs);
        order += "b";
        return r;
    });

    std::vector<Message> msgs;
    chain.run(msgs, [&](auto&) { order += "X"; return make_response(""); });
    CHECK(order == "ABXba");
}

TEST_CASE("SystemPrompt middleware type") {
    auto mw = middleware::SystemPrompt{"Be helpful."};
    std::vector<Message> msgs = {Message::user("hi")};
    auto resp = mw(msgs, [](auto& m) {
        return make_response(m.front().text());
    });
    CHECK(resp.message.text() == "Be helpful.");
    CHECK(msgs.size() == 2);
}

TEST_CASE("Runtime trim_history middleware") {
    auto mw = middleware::trim_history(3);
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::user("1"), Message::assistant("a"),
        Message::user("2"), Message::assistant("b"),
        Message::user("3"),
    };

    mw(msgs, [](auto& m) {
        return make_response(std::to_string(m.size()));
    });
    CHECK(msgs.size() == 4);
    CHECK(msgs.front().role == Role::system);
}

TEST_CASE("Static middleware stack (compile-time chain)") {
    std::string order;

    struct A {
        std::string* order;
        LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
            *order += "A";
            auto r = next(msgs);
            *order += "a";
            return r;
        }
    };
    struct B {
        std::string* order;
        LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
            *order += "B";
            auto r = next(msgs);
            *order += "b";
            return r;
        }
    };

    auto stack = StaticMiddlewareStack{A{&order}, B{&order}};
    std::vector<Message> msgs;
    stack.run(msgs, [&](auto&) { order += "X"; return make_response(""); });
    CHECK(order == "ABXba");
}

TEST_CASE("Retry middleware template") {
    int attempts = 0;
    middleware::Retry<2, 1> retry_mw;

    std::vector<Message> msgs;
    auto resp = retry_mw(msgs, [&](auto&) -> LLMResponse {
        if (++attempts < 3)
            throw APIError(500, "server error");
        return make_response("ok");
    });

    CHECK(resp.message.text() == "ok");
    CHECK(attempts == 3);
}

TEST_CASE("TrimHistory middleware template") {
    middleware::TrimHistory<3> trim_mw;
    std::vector<Message> msgs = {
        Message::system("sys"),
        Message::user("1"), Message::assistant("a"),
        Message::user("2"), Message::assistant("b"),
        Message::user("3"),
    };

    trim_mw(msgs, [](auto& m) {
        return make_response(std::to_string(m.size()));
    });
    CHECK(msgs.size() == 4);
}
