#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/types.hpp>
#include <tiny_agent/core/log.hpp>
#include <sstream>

using namespace tiny_agent;

TEST_CASE("Role string conversion") {
    CHECK(std::string(to_string(Role::system))    == "system");
    CHECK(std::string(to_string(Role::user))      == "user");
    CHECK(std::string(to_string(Role::assistant)) == "assistant");
    CHECK(std::string(to_string(Role::tool))      == "tool");

    CHECK(role_from_string("system")    == Role::system);
    CHECK(role_from_string("assistant") == Role::assistant);
    CHECK(role_from_string("tool")      == Role::tool);
    CHECK(role_from_string("unknown")   == Role::user);
}

TEST_CASE("Message factories") {
    auto sys = Message::system("you are helpful");
    CHECK(sys.role == Role::system);
    CHECK(sys.text() == "you are helpful");
    CHECK_FALSE(sys.has_tool_calls());

    auto usr = Message::user("hello");
    CHECK(usr.role == Role::user);

    auto asst = Message::assistant("hi there");
    CHECK(asst.role == Role::assistant);
    CHECK(asst.text() == "hi there");

    auto tr = Message::tool_result("tc_1", R"({"result": 42})");
    CHECK(tr.role == Role::tool);
    CHECK(tr.tool_call_id.value() == "tc_1");
}

TEST_CASE("Multimodal message") {
    auto img = Message::image("describe this", "https://example.com/img.png", "high");
    CHECK(img.role == Role::user);
    auto* parts = std::get_if<std::vector<ContentPart>>(&img.content);
    REQUIRE(parts != nullptr);
    CHECK(parts->size() == 2);
    CHECK((*parts)[0].type == "text");
    CHECK((*parts)[1].type == "image_url");
    CHECK((*parts)[1].image_url->detail == "high");
}

TEST_CASE("Errors") {
    CHECK_THROWS_AS(throw APIError(429, "rate limited"), APIError);
    CHECK_THROWS_AS(throw ToolError("not found"), ToolError);
    CHECK_THROWS_AS(throw MCPError("connection lost"), MCPError);
    CHECK_THROWS_AS(throw ParseError("bad json"), ParseError);
    CHECK_THROWS_AS(throw ValidationError("invalid"), ValidationError);

    try { throw APIError(500, "server error"); }
    catch (const APIError& e) {
        CHECK(e.status_code == 500);
        CHECK(std::string(e.what()) == "server error");
    }
}

TEST_CASE("Log level filtering") {
    std::ostringstream oss;
    Log log{oss, LogLevel::warn};

    log.debug("test", "should not appear");
    log.info("test", "should not appear");
    log.warn("test", "visible warning");
    log.error("test", "visible error");

    CHECK(oss.str().find("should not appear") == std::string::npos);
    CHECK(oss.str().find("visible warning") != std::string::npos);
    CHECK(oss.str().find("visible error") != std::string::npos);
}

TEST_CASE("Log level off suppresses all") {
    std::ostringstream oss;
    Log log{oss, LogLevel::off};

    log.trace("x", "a");
    log.debug("x", "b");
    log.info("x", "c");
    log.warn("x", "d");
    log.error("x", "e");

    CHECK(oss.str().empty());
}

TEST_CASE("Log format includes level and tag") {
    std::ostringstream oss;
    Log log{oss, LogLevel::trace};

    log.info("my_agent", "hello world");
    CHECK(oss.str() == "[INFO] [my_agent] hello world\n");
}

TEST_CASE("Log set_level changes threshold at runtime") {
    std::ostringstream oss;
    Log log{oss, LogLevel::error};

    log.warn("t", "hidden");
    CHECK(oss.str().empty());

    log.set_level(LogLevel::warn);
    log.warn("t", "now visible");
    CHECK(oss.str().find("now visible") != std::string::npos);
}

TEST_CASE("LLMResponse map") {
    LLMResponse resp{Message::assistant("hello"), {}, "stop", {}};
    auto mapped = resp.map([](const std::string& s) { return s + " world"; });
    CHECK(mapped.message.text() == "hello world");
    CHECK(mapped.finish_reason == "stop");
}
