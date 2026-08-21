#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/tiny_agent.hpp>
#include "mock_model.hpp"

using namespace tiny_agent;
using tiny_agent::test::MockChat;
using tiny_agent::test::MockEmbed;

// ═══════════════════════════════════════════════════════════════════════════
// LLMConfig::merge
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("LLMConfig::merge keeps base fields when the override only sets a key") {
    LLMConfig base;
    base.base_url        = "http://localhost:11434";
    base.timeout_seconds = 5;
    base.headers["X-Trace"] = "yes";
    base.max_tokens      = 256;

    LLMConfig over;
    over.api_key = "sk-new";

    auto merged = LLMConfig::merge(base, over);
    CHECK(merged.api_key == "sk-new");
    CHECK(merged.base_url == "http://localhost:11434");
    CHECK(merged.timeout_seconds == 5);
    CHECK(merged.headers.at("X-Trace") == "yes");
    REQUIRE(merged.max_tokens.has_value());
    CHECK(*merged.max_tokens == 256);
}

TEST_CASE("LLMConfig::merge applies an override that carries no key") {
    LLMConfig base;
    base.api_key    = "sk-base";
    base.temperature = 0.7;

    LLMConfig over;
    over.temperature = 0.1;

    auto merged = LLMConfig::merge(base, over);
    CHECK(merged.api_key == "sk-base");
    REQUIRE(merged.temperature.has_value());
    CHECK(*merged.temperature == doctest::Approx(0.1));
}

// ═══════════════════════════════════════════════════════════════════════════
// MiddlewareChain
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("MiddlewareChain rejects empty callables") {
    MiddlewareChain chain;
    CHECK_THROWS_AS(chain.add(MiddlewareFn{}), Error);

    std::vector<Message> msgs;
    CHECK_THROWS_AS(chain.run(msgs, Next{}), Error);
}

TEST_CASE("MiddlewareChain runs outermost-first and clears") {
    MiddlewareChain chain;
    std::string order;
    chain.add([&](std::vector<Message>& m, Next next) {
        order += "a"; auto r = next(m); order += "A"; return r;
    });
    chain.add([&](std::vector<Message>& m, Next next) {
        order += "b"; auto r = next(m); order += "B"; return r;
    });

    std::vector<Message> msgs = {Message::user("hi")};
    auto r = chain.run(msgs, [&](std::vector<Message>&) {
        order += "t";
        return LLMResponse{Message::assistant("done"), {}, "stop", {}};
    });

    CHECK(order == "abtBA");
    CHECK(r.message.text() == "done");
    CHECK(chain.size() == 2);
    chain.clear();
    CHECK(chain.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// StreamAccumulator
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("StreamAccumulator keeps a truncated tool-argument fragment") {
    StreamAccumulator acc;
    StreamEvent d{StreamEvent::Kind::tool_call_delta};
    d.tool_index = 0;
    d.tool_name  = "search";
    d.tool_args  = R"({"query": "unterm)";
    acc.push(d);
    acc.push(StreamEvent{StreamEvent::Kind::finish, "", -1, "", "", "tool_calls"});

    LLMResponse r;
    REQUIRE_NOTHROW(r = acc.result());
    REQUIRE(r.message.tool_calls.size() == 1);
    CHECK(r.message.tool_calls[0].name == "search");
    CHECK(r.message.tool_calls[0].arguments.contains("_raw_arguments"));
}

TEST_CASE("StreamAccumulator parses well-formed tool arguments") {
    StreamAccumulator acc;
    StreamEvent a{StreamEvent::Kind::tool_call_delta};
    a.tool_index = 0; a.tool_name = "add"; a.tool_args = R"({"a": 1,)";
    acc.push(a);
    StreamEvent b{StreamEvent::Kind::tool_call_delta};
    b.tool_index = 0; b.tool_args = R"( "b": 2})";
    acc.push(b);

    auto r = acc.result();
    REQUIRE(r.message.tool_calls.size() == 1);
    CHECK(r.message.tool_calls[0].arguments["a"] == 1);
    CHECK(r.message.tool_calls[0].arguments["b"] == 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// DeepAgent
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("DeepAgent rejects a non-positive max_iterations") {
    CHECK_THROWS_AS(
        (agents::DeepAgent<MockChat>(MockChat{}, AgentConfig{.max_iterations = 0})),
        Error);
}

TEST_CASE("DeepAgent rejects an empty middleware") {
    AgentConfig cfg;
    cfg.middlewares.push_back(MiddlewareFn{});
    CHECK_THROWS_AS((agents::DeepAgent<MockChat>(MockChat{}, std::move(cfg))), Error);
}

TEST_CASE("DeepAgent::as_tool explains that it needs shared ownership") {
    agents::DeepAgent<MockChat> stack_agent{MockChat{}};
    CHECK_THROWS_AS(stack_agent.as_tool("sub", "delegate"), Error);

    auto shared = make_shared_agent(MockChat{});
    DynamicTool t;
    REQUIRE_NOTHROW(t = shared->as_tool("sub", "delegate"));
    CHECK(t.schema.name == "sub");
}

TEST_CASE("DeepAgent turns a throwing tool into a tool_result the model can read") {
    AgentConfig cfg;
    cfg.tools.push_back(DynamicTool::create("boom", "always fails",
        [](const json&) -> json { throw std::runtime_error("kaboom"); }));
    cfg.tools.push_back(DynamicTool::create("odd", "throws a non-std type",
        [](const json&) -> json { throw 42; }));

    MockChat llm;
    llm.script = {MockChat::tool_call("boom"), MockChat::text("recovered")};

    agents::DeepAgent<MockChat> agent{std::move(llm), std::move(cfg)};
    CHECK(agent.run("go") == "recovered");

    // The second turn's message vector carries the failed tool's result.
    auto& sent = agent.llm().seen.back();
    bool saw_error = false;
    for (auto& m : sent)
        if (m.role == Role::tool && m.text().find("kaboom") != std::string::npos)
            saw_error = true;
    CHECK(saw_error);
}

TEST_CASE("DeepAgent survives a tool that throws a non-std exception") {
    AgentConfig cfg;
    cfg.tools.push_back(DynamicTool::create("odd", "throws an int",
        [](const json&) -> json { throw 42; }));

    MockChat llm;
    llm.script = {MockChat::tool_call("odd"), MockChat::text("still here")};

    agents::DeepAgent<MockChat> agent{std::move(llm), std::move(cfg)};
    CHECK(agent.run("go") == "still here");
}

TEST_CASE("DeepAgent::add_tool rejects unusable tools") {
    agents::DeepAgent<MockChat> agent{MockChat{}};
    CHECK_THROWS_AS(agent.add_tool(DynamicTool{}), ToolError);

    DynamicTool named;
    named.schema.name = "named";
    CHECK_THROWS_AS(agent.add_tool(named), ToolError);

    agent.add_tool(DynamicTool::create("ok", "fine", [](const json&) { return json(1); }));
    CHECK(agent.tool_count() == 1);
}

TEST_CASE("DeepAgent stops at max_iterations") {
    AgentConfig cfg;
    cfg.max_iterations = 2;
    cfg.tools.push_back(DynamicTool::create("loop", "never satisfies",
        [](const json&) { return json("again"); }));

    MockChat llm;
    llm.script = {MockChat::tool_call("loop")};   // repeats forever

    agents::DeepAgent<MockChat> agent{std::move(llm), std::move(cfg)};
    auto out = agent.run("go");
    CHECK(out.find("maximum iterations") != std::string::npos);
    CHECK(agent.llm().calls == 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// Retriever
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Retriever reports an embeddings model that returns the wrong count") {
    MockEmbed embed;
    embed.force_count = 1;
    Retriever r{embed, 2};
    CHECK_THROWS_AS(r.add_documents({"a", "b", "c"}), Error);
}

TEST_CASE("Retriever assigns unique ids across separate add_documents calls") {
    Retriever r{MockEmbed{}, 4};
    r.add_documents({"alpha", "beta"});
    r.add_documents({"gamma"});
    CHECK(r.store().size() == 3);

    auto hits = r.query("alpha", 3);
    std::vector<std::string> ids;
    for (auto& h : hits) ids.push_back(h.id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("Retriever handles an empty add_documents") {
    Retriever r{MockEmbed{}, 4};
    REQUIRE_NOTHROW(r.add_documents({}));
    CHECK(r.store().size() == 0);
}
