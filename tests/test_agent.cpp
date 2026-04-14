#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <tiny_agent/providers/anthropic.hpp>
#include <tiny_agent/providers/gemini.hpp>
#include <libenvpp/env.hpp>
#include <nlohmann/json-schema.hpp>
#include <fstream>
#include <sstream>

using namespace tiny_agent;
using nlohmann::json_schema::json_validator;

// ── .env file loader (sets env vars so libenvpp can read them) ──────────────

static void load_dotenv(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        auto key = line.substr(0, pos);
        auto val = line.substr(pos + 1);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back())))
            val.pop_back();
        setenv(key.c_str(), val.c_str(), 0);
    }
}

// ── API keys via libenvpp (prefixless) ──────────────────────────────────────

struct EnvKeys {
    std::string openai;
    std::string claude;
    std::string gemini;
};

static const EnvKeys& keys() {
    static const EnvKeys k = [] {
        load_dotenv(std::string(PROJECT_SOURCE_DIR) + "/.env");
        return EnvKeys{
            env::get_or<std::string>("OPENAI_API_KEY", ""),
            env::get_or<std::string>("CLAUDE_API_KEY", ""),
            env::get_or<std::string>("GEMINI_API_KEY", ""),
        };
    }();
    return k;
}

// ── Schema validation helper ───────────────────────────────────────────────

static void validate_args(const json& schema, const json& instance) {
    json_validator v;
    v.set_root_schema(schema);
    v.validate(instance);
}

static const json two_operand_schema = {
    {"type", "object"},
    {"properties", {
        {"a", {{"type", "number"}, {"description", "First operand"}}},
        {"b", {{"type", "number"}, {"description", "Second operand"}}}
    }},
    {"required", {"a", "b"}}
};

// ── JSON Schema Validation ─────────────────────────────────────────────────

TEST_CASE("schema: valid arguments pass validation") {
    CHECK_NOTHROW(validate_args(two_operand_schema, {{"a", 17}, {"b", 25}}));
}

TEST_CASE("schema: missing required field is rejected") {
    CHECK_THROWS(validate_args(two_operand_schema, {{"a", 17}}));
}

TEST_CASE("schema: wrong type is rejected") {
    CHECK_THROWS(validate_args(two_operand_schema, {{"a", "NaN"}, {"b", 2}}));
}

// ── OpenAI ──────────────────────────────────────────────────────────────────

TEST_CASE("openai: basic chat") {
    REQUIRE_FALSE(keys().openai.empty());
    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", keys().openai},
        AgentConfig{}
    };
    auto result = agent.run("Reply with exactly: PONG");
    CHECK(result.find("PONG") != std::string::npos);
}

TEST_CASE("openai: tool calling with schema validation") {
    REQUIRE_FALSE(keys().openai.empty());
    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", keys().openai},
        AgentConfig{
            .system_prompt = "Use the add tool to compute the answer.",
            .tools = {
                Tool::create("add", "Add two numbers",
                    [](const json& p) -> json {
                        validate_args(two_operand_schema, p);
                        return p["a"].get<int>() + p["b"].get<int>();
                    }, two_operand_schema)
            },
        }
    };
    CHECK(agent.run("What is 17 + 25?").find("42") != std::string::npos);
}

TEST_CASE("openai: multi-turn chat") {
    REQUIRE_FALSE(keys().openai.empty());
    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", keys().openai},
        AgentConfig{.system_prompt = "You are a concise assistant."}
    };
    auto first = agent.chat("Say hello");
    CHECK_FALSE(first.empty());
    CHECK(agent.history().size() >= 2);

    auto second = agent.chat("Repeat your last message verbatim");
    CHECK_FALSE(second.empty());
    CHECK(agent.history().size() >= 4);
}

TEST_CASE("openai: generation parameters") {
    REQUIRE_FALSE(keys().openai.empty());
    auto llm = LLM<openai>{"gpt-4o-mini", LLMConfig{
        .api_key = keys().openai,
        .temperature = 0.0,
        .max_tokens = 10,
        .seed = 42,
    }};
    auto resp = llm.chat({Message::user("Say hello")});
    CHECK_FALSE(resp.message.text().empty());
    CHECK(resp.usage.contains("total_tokens"));
}

// ── Anthropic ───────────────────────────────────────────────────────────────

TEST_CASE("anthropic: basic chat") {
    REQUIRE_FALSE(keys().claude.empty());
    auto agent = Agent{
        LLM<anthropic>{"claude-sonnet-4-20250514", keys().claude},
        AgentConfig{}
    };
    auto result = agent.run("Reply with exactly: PONG");
    CHECK(result.find("PONG") != std::string::npos);
}

TEST_CASE("anthropic: tool calling with schema validation") {
    REQUIRE_FALSE(keys().claude.empty());
    auto agent = Agent{
        LLM<anthropic>{"claude-sonnet-4-20250514", keys().claude},
        AgentConfig{
            .system_prompt = "Use the multiply tool.",
            .tools = {
                Tool::create("multiply", "Multiply two numbers",
                    [](const json& p) -> json {
                        validate_args(two_operand_schema, p);
                        return p["a"].get<int>() * p["b"].get<int>();
                    }, two_operand_schema)
            },
        }
    };
    CHECK(agent.run("What is 6 * 7?").find("42") != std::string::npos);
}

// ── Gemini ──────────────────────────────────────────────────────────────────

TEST_CASE("gemini: basic chat") {
    REQUIRE_FALSE(keys().gemini.empty());
    auto agent = Agent{
        LLM<gemini>{"gemini-2.0-flash", keys().gemini},
        AgentConfig{}
    };
    auto result = agent.run("Reply with exactly: PONG");
    CHECK(result.find("PONG") != std::string::npos);
}

TEST_CASE("gemini: tool calling with schema validation") {
    REQUIRE_FALSE(keys().gemini.empty());
    auto agent = Agent{
        LLM<gemini>{"gemini-2.0-flash", keys().gemini},
        AgentConfig{
            .system_prompt = "Use the subtract tool to compute the answer. Always use the tool.",
            .tools = {
                Tool::create("subtract", "Subtract b from a",
                    [](const json& p) -> json {
                        validate_args(two_operand_schema, p);
                        return p["a"].get<int>() - p["b"].get<int>();
                    }, two_operand_schema)
            },
        }
    };
    CHECK(agent.run("What is 100 - 58?").find("42") != std::string::npos);
}

// ── Agent Behavior ──────────────────────────────────────────────────────────

TEST_CASE("agent: tool error is handled gracefully") {
    REQUIRE_FALSE(keys().openai.empty());
    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", keys().openai},
        AgentConfig{
            .system_prompt = "Call the fail tool, then report the error you received.",
            .tools = {
                Tool::create("fail", "Always fails",
                    [](const json&) -> json { throw ToolError("kaboom"); },
                    {{"type", "object"}, {"properties", json::object()}})
            },
        }
    };
    auto result = agent.run("Call the fail tool");
    CHECK_FALSE(result.empty());
}

TEST_CASE("agent: AnyLLM type erasure") {
    REQUIRE_FALSE(keys().openai.empty());
    AnyLLM any{LLM<openai>{"gpt-4o-mini", keys().openai}};
    CHECK(any.model_name() == "gpt-4o-mini");

    auto resp = any.chat({Message::user("Reply with exactly: ERASED")});
    CHECK(resp.message.text().find("ERASED") != std::string::npos);
}

TEST_CASE("agent: Agent<AnyLLM> works") {
    REQUIRE_FALSE(keys().openai.empty());
    AnyLLM any{LLM<openai>{"gpt-4o-mini", keys().openai}};
    auto agent = Agent{std::move(any), AgentConfig{}};
    auto result = agent.run("Reply with exactly: DYNAMIC");
    CHECK(result.find("DYNAMIC") != std::string::npos);
}

TEST_CASE("agent: Log captures output at debug level") {
    REQUIRE_FALSE(keys().openai.empty());
    std::ostringstream oss;
    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", keys().openai},
        AgentConfig{},
        Log{oss, LogLevel::debug}
    };
    agent.run("Say hello");
    CHECK(oss.str().find("iteration 1") != std::string::npos);
    CHECK(oss.str().find("done") != std::string::npos);
}

TEST_CASE("agent: cross-provider sub-agent delegation") {
    REQUIRE_FALSE(keys().openai.empty());
    REQUIRE_FALSE(keys().gemini.empty());

    auto worker = make_shared_agent(
        LLM<gemini>{"gemini-2.0-flash", keys().gemini},
        AgentConfig{
            .name = "gemini_worker",
            .system_prompt = "Answer factual questions concisely.",
        }
    );

    auto manager = make_shared_agent(
        LLM<openai>{"gpt-4o-mini", keys().openai},
        AgentConfig{
            .name = "manager",
            .system_prompt = "Delegate questions to gemini_worker and relay the answer.",
            .tools = {agent_as_tool(worker, "gemini_worker", "Answer factual questions")},
        }
    );

    auto result = manager->run("What is the chemical symbol for gold?");
    CHECK(result.find("Au") != std::string::npos);
}
