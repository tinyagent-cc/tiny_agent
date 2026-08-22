#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rete/rete.hpp>
#include <tiny_agent/integrations/rete_convert.hpp>
#include <tiny_agent/middleware/reflex.hpp>

using namespace tiny_agent;

TEST_CASE("rete_cpp compiles under the C++20 test build and fires a rule") {
    rete::ReteEngine eng;
    int fired = 0;
    eng.add_rule("smoke")
        .when(std::string("x"), std::string("is"), std::string("on"))
        .then([&](rete::ReteEngine&, const rete::Bindings&) { ++fired; })
        .build();
    eng.assert_fact(std::string("x"), std::string("is"), std::string("on"));
    eng.run();
    CHECK(fired == 1);
}

TEST_CASE("json <-> rete::Value round-trips scalars") {
    using tiny_agent::integrations::to_rete_value;
    using tiny_agent::integrations::from_rete_value;
    using json = nlohmann::json;

    CHECK(std::get<std::string>(to_rete_value(json("hi"))) == "hi");
    CHECK(std::get<int64_t>(to_rete_value(json(42))) == 42);
    CHECK(std::get<double>(to_rete_value(json(2.5))) == 2.5);
    CHECK(std::get<bool>(to_rete_value(json(true))) == true);
    CHECK(std::holds_alternative<std::monostate>(to_rete_value(json())));
    // non-scalar collapses to its compact dump, so no fact is ever lost silently
    CHECK(std::get<std::string>(to_rete_value(json::parse(R"({"a":1})"))) == R"({"a":1})");
    CHECK(std::get<int64_t>(to_rete_value(json(5000000000ULL))) == 5000000000LL);

    CHECK(from_rete_value(rete::Value{std::string("hi")}) == json("hi"));
    CHECK(from_rete_value(rete::Value{int64_t{42}}) == json(42));
    CHECK(from_rete_value(rete::Value{true}) == json(true));
    CHECK(from_rete_value(rete::Value{std::monostate{}}) == json());
    CHECK(from_rete_value(rete::Value{2.5}) == json(2.5));
}

static LLMResponse model_says(const std::string& text) {
    return {Message::assistant(text), {}, "stop", {}};
}

TEST_CASE("reflex short-circuits the model when a rule fires") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("ping")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().respond("pong"); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_facts = [](const std::vector<Message>& msgs) {
            return std::vector<middleware::Fact>{
                {std::string("msg"), std::string("text"), msgs.back().text()}};
        }}));

    int model_calls = 0;
    std::vector<Message> msgs = {Message::user("ping")};
    auto resp = chain.run(msgs, [&](auto&) { ++model_calls; return model_says("expensive"); });

    CHECK(model_calls == 0);
    CHECK(resp.message.text() == "pong");
    CHECK(resp.finish_reason == "reflex");
}

TEST_CASE("reflex passes through when no rule fires") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("ping")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().respond("pong"); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_facts = [](const std::vector<Message>& msgs) {
            return std::vector<middleware::Fact>{
                {std::string("msg"), std::string("text"), msgs.back().text()}};
        }}));

    int model_calls = 0;
    std::vector<Message> msgs = {Message::user("something hard")};
    auto resp = chain.run(msgs, [&](auto&) { ++model_calls; return model_says("thought"); });

    CHECK(model_calls == 1);
    CHECK(resp.message.text() == "thought");
}

TEST_CASE("reflex can emit a tool call directly") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("wink")
        .when(std::string("event"), std::string("name"), std::string("button.press"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) {
            rx.outcome().call_tool("express", {{"emotion", "wink"}});
        })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_facts = [](const std::vector<Message>& msgs) {
            return std::vector<middleware::Fact>{
                {std::string("event"), std::string("name"), msgs.back().text()}};
        }}));

    std::vector<Message> msgs = {Message::user("button.press")};
    auto resp = chain.run(msgs, [](auto&) { return model_says("unused"); });

    REQUIRE(resp.message.tool_calls.size() == 1);
    CHECK(resp.message.tool_calls[0].name == "express");
    CHECK(resp.message.tool_calls[0].arguments["emotion"] == "wink");
    CHECK(resp.finish_reason == "reflex");
}

TEST_CASE("transient facts do not leak between runs") {
    middleware::ReflexEngine rx;
    int fires = 0;
    rx.engine().add_rule("count")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([&](rete::ReteEngine&, const rete::Bindings&) { ++fires; rx.outcome().respond("pong"); })
        .build();

    auto mw = rx.middleware({
        .extract_facts = [](const std::vector<Message>& msgs) {
            return std::vector<middleware::Fact>{
                {std::string("msg"), std::string("text"), msgs.back().text()}};
        }});

    MiddlewareChain chain; chain.add(mw);
    std::vector<Message> a = {Message::user("ping")};
    chain.run(a, [](auto&) { return model_says("x"); });
    std::vector<Message> b = {Message::user("quiet")};
    auto r2 = chain.run(b, [](auto&) { return model_says("model"); });

    CHECK(fires == 1);
    CHECK(r2.message.text() == "model");
    CHECK(rx.engine().wme_count() == 0);   // transients retracted
}

TEST_CASE("the same reflex fires again on a later identical event") {
    middleware::ReflexEngine rx;
    int fires = 0;
    rx.engine().add_rule("re")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([&](rete::ReteEngine&, const rete::Bindings&) { ++fires; rx.outcome().respond("pong"); })
        .build();
    auto mw = rx.middleware({.extract_facts = [](const std::vector<Message>& msgs) {
        return std::vector<middleware::Fact>{{std::string("msg"), std::string("text"), msgs.back().text()}}; }});
    MiddlewareChain chain; chain.add(mw);
    std::vector<Message> a = {Message::user("ping")};
    chain.run(a, [](auto&) { return model_says("x"); });
    std::vector<Message> b = {Message::user("ping")};
    chain.run(b, [](auto&) { return model_says("x"); });
    CHECK(fires == 2);
}

TEST_CASE("transient facts are retracted even when a rule action throws") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("boom")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([](rete::ReteEngine&, const rete::Bindings&) { throw Error("boom"); })
        .build();

    auto mw = rx.middleware({
        .extract_facts = [](const std::vector<Message>& msgs) {
            return std::vector<middleware::Fact>{
                {std::string("msg"), std::string("text"), msgs.back().text()}};
        }});

    MiddlewareChain chain; chain.add(mw);
    std::vector<Message> a = {Message::user("ping")};
    CHECK_THROWS(chain.run(a, [](auto&) { return model_says("x"); }));
    CHECK(rx.engine().wme_count() == 0);

    // engine still behaves on a later run: no spurious match from leaked facts
    std::vector<Message> b = {Message::user("quiet")};
    auto resp = chain.run(b, [](auto&) { return model_says("model"); });
    CHECK(resp.message.text() == "model");
}
