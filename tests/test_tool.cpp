#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/tool.hpp>

using namespace tiny_agent;

TEST_CASE("DynamicTool creation and invocation") {
    auto tool = DynamicTool::create("add", "Add two numbers",
        [](const json& p) -> json {
            return p["a"].get<int>() + p["b"].get<int>();
        },
        {{"type", "object"}, {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}});

    CHECK(tool.schema.name == "add");
    CHECK(tool.schema.description == "Add two numbers");

    auto result = tool({{"a", 3}, {"b", 4}});
    CHECK(result.get<int>() == 7);
}

TEST_CASE("DynamicTool with no handler throws") {
    DynamicTool empty{{"empty", "no handler", {}}, nullptr};
    CHECK_THROWS_AS(empty(json::object()), ToolError);
}

TEST_CASE("ToolRegistry") {
    ToolRegistry reg;

    reg.add(DynamicTool::create("mul", "Multiply",
        [](const json& p) -> json { return p["a"].get<int>() * p["b"].get<int>(); }));
    reg.add(DynamicTool::create("neg", "Negate",
        [](const json& p) -> json { return -p["x"].get<int>(); }));

    CHECK(reg.size() == 2);
    CHECK(reg.has("mul"));
    CHECK_FALSE(reg.has("div"));

    auto schemas = reg.schemas();
    CHECK(schemas.size() == 2);

    auto r = reg.execute("mul", {{"a", 5}, {"b", 3}});
    CHECK(r.get<int>() == 15);

    CHECK_THROWS_AS(reg.execute("div", {}), ToolError);
}

TEST_CASE("DynamicTool::create respects std::invocable concept") {
    auto tool = DynamicTool::create("greet", "Say hello",
        [](const json& p) -> json {
            return "Hello, " + p["name"].get<std::string>() + "!";
        });

    auto result = tool({{"name", "World"}});
    CHECK(result.get<std::string>() == "Hello, World!");
}
