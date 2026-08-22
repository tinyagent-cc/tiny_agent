#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rete/rete.hpp>
#include <tiny_agent/integrations/rete_convert.hpp>
#include <tiny_agent/middleware/reflex.hpp>
#include <tiny_agent/tools/rete_tool.hpp>

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

static LLMResponse model_calls_tool(const std::string& tool, json args) {
    Message m = Message::assistant("");
    m.tool_calls.push_back({"call-0", tool, std::move(args)});
    return {std::move(m), {}, "tool_calls", {}};
}

TEST_CASE("guardrail vetoes a forbidden tool call") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("no-delete")
        .when(std::string("?c"), std::string("tool"), std::string("delete_everything"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) {
            rx.outcome().veto(0, "destructive tool blocked");
        })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_response_facts = [](const LLMResponse& r) {
            std::vector<middleware::Fact> facts;
            for (size_t i = 0; i < r.message.tool_calls.size(); ++i)
                facts.push_back({std::string("call-") + std::to_string(i),
                                 std::string("tool"), r.message.tool_calls[i].name});
            return facts;
        }}));

    std::vector<Message> msgs = {Message::user("clean up")};
    auto resp = chain.run(msgs, [](auto&) {
        return model_calls_tool("delete_everything", {{"path", "/"}}); });

    CHECK(resp.message.tool_calls.empty());
    CHECK(resp.finish_reason == "reflex_guardrail");
    CHECK(resp.raw["reflex_vetoes"][0]["tool"] == "delete_everything");
}

TEST_CASE("guardrail rewrites an argument and keeps the call") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("cap-brightness")
        .when(std::string("call-0"), std::string("tool"), std::string("led"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) {
            rx.outcome().replace_arg(0, "r", 32);
        })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_response_facts = [](const LLMResponse& r) {
            std::vector<middleware::Fact> facts;
            for (size_t i = 0; i < r.message.tool_calls.size(); ++i)
                facts.push_back({std::string("call-") + std::to_string(i),
                                 std::string("tool"), r.message.tool_calls[i].name});
            return facts;
        }}));

    std::vector<Message> msgs = {Message::user("mood light")};
    auto resp = chain.run(msgs, [](auto&) {
        return model_calls_tool("led", {{"r", 255}, {"g", 0}, {"b", 0}}); });

    REQUIRE(resp.message.tool_calls.size() == 1);
    CHECK(resp.message.tool_calls[0].arguments["r"] == 32);
    CHECK(resp.message.tool_calls[0].arguments["g"] == 0);
    CHECK(resp.finish_reason == "tool_calls");   // untouched: not a veto
}

TEST_CASE("a clean response passes the guardrail untouched") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("no-delete")
        .when(std::string("?c"), std::string("tool"), std::string("delete_everything"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().veto(0, "blocked"); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_response_facts = [](const LLMResponse& r) {
            std::vector<middleware::Fact> facts;
            for (size_t i = 0; i < r.message.tool_calls.size(); ++i)
                facts.push_back({std::string("call-") + std::to_string(i),
                                 std::string("tool"), r.message.tool_calls[i].name});
            return facts;
        }}));

    std::vector<Message> msgs = {Message::user("hi")};
    auto resp = chain.run(msgs, [](auto&) {
        return model_calls_tool("express", {{"emotion", "happy"}}); });

    REQUIRE(resp.message.tool_calls.size() == 1);
    CHECK(resp.message.tool_calls[0].name == "express");
    CHECK_FALSE(resp.raw.contains("reflex_vetoes"));
}

TEST_CASE("guardrail vetoes multiple calls back-to-front and the middle call survives") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("no-delete-0")
        .when(std::string("call-0"), std::string("tool"), std::string("delete_everything"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().veto(0, "blocked"); })
        .build();
    rx.engine().add_rule("no-delete-2")
        .when(std::string("call-2"), std::string("tool"), std::string("delete_everything"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().veto(2, "blocked"); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_response_facts = [](const LLMResponse& r) {
            std::vector<middleware::Fact> facts;
            for (size_t i = 0; i < r.message.tool_calls.size(); ++i)
                facts.push_back({std::string("call-") + std::to_string(i),
                                 std::string("tool"), r.message.tool_calls[i].name});
            return facts;
        }}));

    std::vector<Message> msgs = {Message::user("clean up")};
    auto resp = chain.run(msgs, [](auto&) {
        Message m = Message::assistant("");
        m.tool_calls.push_back({"call-0", "delete_everything", {{"path", "/a"}}});
        m.tool_calls.push_back({"call-1", "keep_me", {{"x", 1}}});
        m.tool_calls.push_back({"call-2", "delete_everything", {{"path", "/b"}}});
        return LLMResponse{std::move(m), {}, "tool_calls", {}};
    });

    REQUIRE(resp.message.tool_calls.size() == 1);
    CHECK(resp.message.tool_calls[0].name == "keep_me");
    CHECK(resp.message.tool_calls[0].arguments["x"] == 1);
    CHECK(resp.raw["reflex_vetoes"].size() == 2);
}

TEST_CASE("guardrail applies an arg replacement on the call that survives a veto") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("no-delete")
        .when(std::string("call-0"), std::string("tool"), std::string("delete_everything"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().veto(0, "blocked"); })
        .build();
    rx.engine().add_rule("cap-brightness")
        .when(std::string("call-1"), std::string("tool"), std::string("led"))
        .then([&rx](rete::ReteEngine&, const rete::Bindings&) { rx.outcome().replace_arg(1, "r", 32); })
        .build();

    MiddlewareChain chain;
    chain.add(rx.middleware({
        .extract_response_facts = [](const LLMResponse& r) {
            std::vector<middleware::Fact> facts;
            for (size_t i = 0; i < r.message.tool_calls.size(); ++i)
                facts.push_back({std::string("call-") + std::to_string(i),
                                 std::string("tool"), r.message.tool_calls[i].name});
            return facts;
        }}));

    std::vector<Message> msgs = {Message::user("mood light, then clean up")};
    auto resp = chain.run(msgs, [](auto&) {
        Message m = Message::assistant("");
        m.tool_calls.push_back({"call-0", "delete_everything", {{"path", "/"}}});
        m.tool_calls.push_back({"call-1", "led", {{"r", 255}, {"g", 0}}});
        return LLMResponse{std::move(m), {}, "tool_calls", {}};
    });

    REQUIRE(resp.message.tool_calls.size() == 1);
    CHECK(resp.message.tool_calls[0].name == "led");
    CHECK(resp.message.tool_calls[0].arguments["r"] == 32);
    CHECK(resp.raw["reflex_vetoes"][0]["tool"] == "delete_everything");
}

TEST_CASE("max_cycles bounds a self-perpetuating rule pair and the middleware still returns") {
    middleware::ReflexEngine rx;
    int bumps = 0;
    // "bump" fires on a "n" fact and asserts a matching "m" fact; "relay"
    // fires on that "m" fact and asserts the next "n" fact, which retriggers
    // "bump" — an unbounded ping-pong with no natural fixed point. Only
    // max_cycles keeps engine_.run() from looping forever.
    rx.engine().add_rule("bump")
        .when(std::string("counter"), std::string("n"), std::string("?v"))
        .then([&](rete::ReteEngine& e, const rete::Bindings& b) {
            ++bumps;
            e.assert_fact(std::string("counter"), std::string("m"), b.at("?v"));
        })
        .build();
    rx.engine().add_rule("relay")
        .when(std::string("counter"), std::string("m"), std::string("?v"))
        .then([&](rete::ReteEngine& e, const rete::Bindings& b) {
            int64_t v = std::get<int64_t>(b.at("?v"));
            e.assert_fact(std::string("counter"), std::string("n"), v + 1);
        })
        .build();

    auto mw = rx.middleware({
        .extract_facts = [](const std::vector<Message>&) {
            return std::vector<middleware::Fact>{
                {std::string("counter"), std::string("n"), static_cast<int64_t>(0)}};
        },
        .max_cycles = 8});

    MiddlewareChain chain; chain.add(mw);
    std::vector<Message> msgs = {Message::user("go")};
    auto resp = chain.run(msgs, [](auto&) { return model_says("model"); });

    // Terminated instead of hanging, and stayed within the cycle budget.
    CHECK(bumps > 0);
    CHECK(bumps <= 8);
    // Neither rule calls respond()/call_tool(), so the reflex phase never
    // triggers and the model still runs.
    CHECK(resp.message.text() == "model");
}

TEST_CASE("message_facts describes the last non-system message") {
    std::vector<Message> msgs = {Message::system("sys"), Message::user("hello world")};
    auto facts = middleware::message_facts(msgs);
    REQUIRE(facts.size() == 2);
    CHECK(std::get<std::string>(facts[0][0]) == "msg");
    CHECK(std::get<std::string>(facts[0][1]) == "role");
    CHECK(std::get<std::string>(facts[0][2]) == "user");
    CHECK(std::get<std::string>(facts[1][1]) == "text");
    CHECK(std::get<std::string>(facts[1][2]) == "hello world");
}

TEST_CASE("tool_call_facts flattens calls and scalar args") {
    auto resp = model_calls_tool("led", {{"r", 255}, {"nested", {{"x", 1}}}});
    auto facts = middleware::tool_call_facts(resp);
    // nlohmann::json objects iterate in sorted key order, so "arg:nested"
    // comes out before "arg:r": ("call-0","tool","led"), ("call-0","index",0),
    // ("call-0","arg:nested","{\"x\":1}"), ("call-0","arg:r",255)
    REQUIRE(facts.size() == 4);
    CHECK(std::get<std::string>(facts[0][2]) == "led");
    CHECK(std::get<int64_t>(facts[1][2]) == 0);
}

TEST_CASE("rete_tool derives facts from a rulebase") {
    auto tool = tools::rete_tool({
        .name = "animal_expert",
        .description = "Classify animals from observed traits.",
        .setup = [](rete::ReteEngine& eng) {
            eng.add_rule("mammal")
                .when(std::string("?x"), std::string("has"), std::string("fur"))
                .then([&eng](rete::ReteEngine& e, const rete::Bindings& b) {
                    e.assert_fact(b.at("?x"), std::string("is"), std::string("mammal"));
                })
                .build();
        }});

    CHECK(tool.schema.name == "animal_expert");
    auto out = tool({{"facts", {{"rex", "has", "fur"}}}});
    bool derived = false;
    for (auto& f : out["facts"])
        if (f[0] == "rex" && f[1] == "is" && f[2] == "mammal") derived = true;
    CHECK(derived);
}

TEST_CASE("rete_tool is stateless across invocations") {
    auto tool = tools::rete_tool({
        .name = "t", .description = "d",
        .setup = [](rete::ReteEngine&) {}});
    tool({{"facts", {{"a", "b", "c"}}}});
    auto out = tool({{"facts", json::array()}});
    CHECK(out["facts"].empty());
}

TEST_CASE("rete_tool with no setup throws ToolError") {
    CHECK_THROWS_AS(tools::rete_tool({.name = "t", .description = "d"}), ToolError);
}

TEST_CASE("rete_tool rejects a malformed fact") {
    auto tool = tools::rete_tool({
        .name = "t", .description = "d",
        .setup = [](rete::ReteEngine&) {}});
    CHECK_THROWS_AS(tool({{"facts", {{"only", "two"}}}}), ToolError);
}
