# Reflex Middleware (rete_cpp integration) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** tiny_agent gains `middleware/reflex.hpp` (pre-LLM reflex short-circuit + post-LLM tool-call guardrail backed by rete_cpp) and `tools/rete_tool.hpp` (a ruleset as a callable tool), fully unit-tested host-side.

**Architecture:** Optional-integration-header pattern already used for vector stores: the headers compile only when rete_cpp is on the include path; rete_cpp never learns about tiny_agent. A `ReflexEngine` owns a `rete::ReteEngine` plus a per-run `ReflexOutcome` that rule actions write to; the middleware asserts transient facts, runs the network, and either short-circuits (reflex), edits the response (guardrail), or passes through.

**Tech Stack:** C++20 (tiny_agent), C++17 headers (rete_cpp), doctest, CMake + vcpkg, FetchContent for rete_cpp.

**Spec:** `docs/superpowers/specs/2026-08-22-pip-companion-design.md`, section "The reflex integration".

## Global Constraints

- Repo: `~/git/tiny_agent_cpp`, branch `feat/reflex-middleware` off `main`.
- rete_cpp is consumed header-only from `https://github.com/tinyagent-cc/rete_cpp`; tiny_agent may include rete headers, never the reverse.
- New public API lives in `tiny_agent::middleware` (reflex) and `tiny_agent::tools` (rete_tool); shared conversion helpers in `tiny_agent::integrations`.
- All new tests are offline doctest cases added to the existing `UNIT_TESTS`-style wiring in `tests/CMakeLists.txt`, guarded by the rete option (Task 1).
- Match house style: `#pragma once`, config-struct + factory returning `MiddlewareFn`, comments only for non-obvious constraints.
- Every commit ends with the two-line trailer:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01H7hNgsKhpDyCMmrMbSLSfi`

---

### Task 1: Build wiring for rete_cpp

**Files:**
- Modify: `tests/CMakeLists.txt`
- Create: `tests/test_reflex.cpp` (compile-smoke only in this task)

**Interfaces:**
- Produces: CMake cache options `TINY_AGENT_RETE_DIR` (local path to a rete_cpp checkout) and `TINY_AGENT_FETCH_RETE` (FetchContent from GitHub, pinned); a `test_reflex` target that later tasks extend. Either option defines compile flag availability; when neither is set, `test_reflex` is skipped and the suite stays fully offline.

- [ ] **Step 1: Write the compile-smoke test**

`tests/test_reflex.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rete/rete.hpp>

TEST_CASE("rete_cpp compiles under the C++20 test build and fires a rule") {
    rete::ReteEngine eng;
    int fired = 0;
    eng.add_rule("smoke")
        .when(std::string("x"), std::string("is"), std::string("on"))
        .then([&](rete::ReteEngine&, rete::Bindings&) { ++fired; })
        .build();
    eng.assert_fact(std::string("x"), std::string("is"), std::string("on"));
    eng.run();
    CHECK(fired == 1);
}
```

Note: `rete::Bindings` — confirm the exact action signature from `~/git/rete_cpp/include/rete/production.hpp` (`Action` is declared there); if the action takes `(ReteEngine&, Bindings)` by value or const-ref, match it exactly in this file and everywhere below.

- [ ] **Step 2: Wire CMake**

In `tests/CMakeLists.txt`, after the existing `find_package` lines:

```cmake
set(TINY_AGENT_RETE_DIR "" CACHE PATH "Local rete_cpp checkout (header-only)")
option(TINY_AGENT_FETCH_RETE "Fetch rete_cpp from GitHub for reflex tests" OFF)

set(RETE_INCLUDE_DIR "")
if(TINY_AGENT_RETE_DIR)
  set(RETE_INCLUDE_DIR "${TINY_AGENT_RETE_DIR}/include")
elseif(TINY_AGENT_FETCH_RETE)
  include(FetchContent)
  FetchContent_Declare(rete_cpp
    GIT_REPOSITORY https://github.com/tinyagent-cc/rete_cpp.git
    GIT_TAG <SHA>)   # run: git ls-remote https://github.com/tinyagent-cc/rete_cpp.git main
                     # and paste the full 40-char SHA here before committing
  FetchContent_MakeAvailable(rete_cpp)
  set(RETE_INCLUDE_DIR "${rete_cpp_SOURCE_DIR}/include")
endif()

if(RETE_INCLUDE_DIR)
  add_executable(test_reflex test_reflex.cpp)
  target_include_directories(test_reflex PRIVATE ${RETE_INCLUDE_DIR})
  target_link_libraries(test_reflex PRIVATE tiny_agent doctest::doctest)
  add_test(NAME test_reflex COMMAND test_reflex)
endif()
```

Mirror however the existing unit tests link (`grep -n 'add_executable\|target_link_libraries' tests/CMakeLists.txt` and copy that pattern — the block above is the intent, the existing pattern is the law).

- [ ] **Step 3: Configure and run**

```bash
cmake -S . -B build -DTINY_AGENT_RETE_DIR=$HOME/git/rete_cpp
cmake --build build --target test_reflex && ctest --test-dir build -R test_reflex --output-on-failure
```

Expected: PASS. If rete_cpp fails to compile under `-std=c++20`, fix nothing silently — record the error, it is a spec-level finding.

- [ ] **Step 4: Also verify the no-rete configure still works**

```bash
cmake -S . -B build-vanilla && cmake --build build-vanilla --target test_middleware
```

Expected: configures and builds with `test_reflex` absent.

- [ ] **Step 5: Commit**

```bash
git add tests/CMakeLists.txt tests/test_reflex.cpp
git commit -m "test: build wiring for optional rete_cpp integration"
```

### Task 2: Value conversion helpers

**Files:**
- Create: `include/tiny_agent/integrations/rete_convert.hpp`
- Test: `tests/test_reflex.cpp` (append cases)

**Interfaces:**
- Produces: `tiny_agent::integrations::to_rete_value(const json&) -> rete::Value` and `from_rete_value(const rete::Value&) -> json`. Later tasks call both.

- [ ] **Step 1: Write the failing tests** (append to `tests/test_reflex.cpp`)

```cpp
#include <tiny_agent/integrations/rete_convert.hpp>

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

    CHECK(from_rete_value(rete::Value{std::string("hi")}) == json("hi"));
    CHECK(from_rete_value(rete::Value{int64_t{42}}) == json(42));
    CHECK(from_rete_value(rete::Value{true}) == json(true));
    CHECK(from_rete_value(rete::Value{std::monostate{}}) == json());
}
```

- [ ] **Step 2: Run to verify failure** — `cmake --build build --target test_reflex` — expected: compile error, header missing.

- [ ] **Step 3: Implement**

`include/tiny_agent/integrations/rete_convert.hpp`:

```cpp
#pragma once
#include "../core/types.hpp"
#include <rete/types.hpp>

namespace tiny_agent::integrations {

inline rete::Value to_rete_value(const json& j) {
    if (j.is_null())            return std::monostate{};
    if (j.is_boolean())         return j.get<bool>();
    if (j.is_number_integer())  return j.get<int64_t>();
    if (j.is_number_unsigned()) return static_cast<int64_t>(j.get<uint64_t>());
    if (j.is_number_float())    return j.get<double>();
    if (j.is_string())          return j.get<std::string>();
    return j.dump();
}

inline json from_rete_value(const rete::Value& v) {
    return std::visit([](auto&& arg) -> json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) return json();
        else return json(arg);
    }, v);
}

} // namespace tiny_agent::integrations
```

- [ ] **Step 4: Run to verify pass** — `ctest --test-dir build -R test_reflex --output-on-failure` — expected: PASS.

- [ ] **Step 5: Commit** — `git add include/tiny_agent/integrations/rete_convert.hpp tests/test_reflex.cpp && git commit -m "feat: json <-> rete::Value conversion helpers"`

### Task 3: ReflexEngine with the pre-LLM reflex path

**Files:**
- Create: `include/tiny_agent/middleware/reflex.hpp`
- Test: `tests/test_reflex.cpp` (append)

**Interfaces:**
- Consumes: `to_rete_value` (Task 2); `MiddlewareFn`, `Next`, `LLMResponse`, `Message`, `ToolCall` from core.
- Produces, exactly (Tasks 4-6 and Pip's brain rely on these names):

```cpp
namespace tiny_agent::middleware {
  using Fact = std::array<rete::Value, 3>;

  class ReflexOutcome {
  public:
    void respond(std::string text);
    void call_tool(std::string name, json args);
    void veto(int tool_call_index, std::string reason);
    void replace_arg(int tool_call_index, std::string key, json value);
    bool triggered() const;
    void reset();
    LLMResponse to_response() const;              // finish_reason "reflex"
    void apply_guardrails(LLMResponse& resp) const;
  };

  struct ReflexConfig {
    std::function<std::vector<Fact>(const std::vector<Message>&)> extract_facts;
    std::function<std::vector<Fact>(const LLMResponse&)> extract_response_facts;
    int max_cycles = 256;
  };

  class ReflexEngine {
  public:
    rete::ReteEngine& engine();                   // register rules here
    ReflexOutcome& outcome();                     // rule actions capture this
    MiddlewareFn middleware(ReflexConfig cfg);    // engine must outlive the chain
  };
}
```

- [ ] **Step 1: Write the failing tests** (append)

```cpp
#include <tiny_agent/middleware/reflex.hpp>
using namespace tiny_agent;

static LLMResponse model_says(const std::string& text) {
    return {Message::assistant(text), {}, "stop", {}};
}

TEST_CASE("reflex short-circuits the model when a rule fires") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("ping")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([&rx](rete::ReteEngine&, rete::Bindings&) { rx.outcome().respond("pong"); })
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
        .then([&rx](rete::ReteEngine&, rete::Bindings&) { rx.outcome().respond("pong"); })
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
        .then([&rx](rete::ReteEngine&, rete::Bindings&) {
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
        .then([&](rete::ReteEngine&, rete::Bindings&) { ++fires; rx.outcome().respond("pong"); })
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
```

- [ ] **Step 2: Run to verify failure** — compile error, header missing.

- [ ] **Step 3: Implement** `include/tiny_agent/middleware/reflex.hpp`:

```cpp
#pragma once
#include "../core/middleware.hpp"
#include "../integrations/rete_convert.hpp"
#include <rete/rete.hpp>
#include <array>
#include <map>
#include <tuple>

namespace tiny_agent::middleware {

using Fact = std::array<rete::Value, 3>;

class ReflexOutcome {
    std::optional<std::string> text_;
    std::vector<ToolCall> calls_;
    std::map<int, std::string> vetoes_;
    std::vector<std::tuple<int, std::string, json>> arg_replacements_;
    int next_id_ = 0;
public:
    void respond(std::string text) { text_ = std::move(text); }

    void call_tool(std::string name, json args) {
        calls_.push_back({"reflex-" + std::to_string(next_id_++),
                          std::move(name), std::move(args)});
    }

    void veto(int tool_call_index, std::string reason) {
        vetoes_[tool_call_index] = std::move(reason);
    }

    void replace_arg(int tool_call_index, std::string key, json value) {
        arg_replacements_.emplace_back(tool_call_index, std::move(key), std::move(value));
    }

    bool triggered() const { return text_.has_value() || !calls_.empty(); }
    bool has_guardrails() const { return !vetoes_.empty() || !arg_replacements_.empty(); }

    void reset() {
        text_.reset(); calls_.clear(); vetoes_.clear();
        arg_replacements_.clear(); next_id_ = 0;
    }

    LLMResponse to_response() const {
        Message m = Message::assistant(text_.value_or(""));
        m.tool_calls = calls_;
        return {std::move(m), json{{"reflex", true}}, "reflex", json{{"reflex", true}}};
    }

    void apply_guardrails(LLMResponse& resp) const {
        auto& calls = resp.message.tool_calls;
        for (auto& [idx, key, value] : arg_replacements_)
            if (idx >= 0 && idx < static_cast<int>(calls.size()))
                calls[static_cast<size_t>(idx)].arguments[key] = value;

        if (!vetoes_.empty()) {
            json veto_log = json::array();
            // erase back-to-front so indices stay valid
            for (auto it = vetoes_.rbegin(); it != vetoes_.rend(); ++it) {
                if (it->first >= 0 && it->first < static_cast<int>(calls.size())) {
                    veto_log.push_back({{"tool", calls[static_cast<size_t>(it->first)].name},
                                        {"reason", it->second}});
                    calls.erase(calls.begin() + it->first);
                }
            }
            resp.raw["reflex_vetoes"] = veto_log;
            // a response that was only vetoed calls must not read as a silent
            // empty answer to the agent loop
            if (calls.empty() && resp.message.text().empty() && !veto_log.empty()) {
                std::string note = "Blocked by reflex guardrail:";
                for (auto& v : veto_log)
                    note += " " + v["tool"].get<std::string>() +
                            " (" + v["reason"].get<std::string>() + ");";
                resp.message.content = note;
                resp.finish_reason = "reflex_guardrail";
            }
        }
    }
};

struct ReflexConfig {
    std::function<std::vector<Fact>(const std::vector<Message>&)> extract_facts;
    std::function<std::vector<Fact>(const LLMResponse&)> extract_response_facts;
    int max_cycles = 256;
};

// Owns the rete network and the per-run outcome. Register rules on engine();
// rule actions capture outcome() by reference. The ReflexEngine must outlive
// every chain its middleware() was added to. Not thread-safe: one concurrent
// run at a time.
class ReflexEngine {
    rete::ReteEngine engine_;
    ReflexOutcome outcome_;
    std::vector<rete::WmePtr> transient_;

    void assert_transient(const std::vector<Fact>& facts) {
        for (auto& f : facts)
            transient_.push_back(engine_.assert_fact(f[0], f[1], f[2]));
    }
    void retract_transient() {
        for (auto& w : transient_) engine_.retract_fact(w);
        transient_.clear();
    }

public:
    rete::ReteEngine& engine() { return engine_; }
    ReflexOutcome& outcome() { return outcome_; }

    MiddlewareFn middleware(ReflexConfig cfg) {
        return [this, cfg = std::move(cfg)](std::vector<Message>& msgs, Next next) -> LLMResponse {
            if (cfg.extract_facts) {
                outcome_.reset();
                assert_transient(cfg.extract_facts(msgs));
                engine_.run(cfg.max_cycles);
                retract_transient();
                if (outcome_.triggered()) return outcome_.to_response();
            }
            auto resp = next(msgs);
            if (cfg.extract_response_facts && resp.message.has_tool_calls()) {
                outcome_.reset();
                assert_transient(cfg.extract_response_facts(resp));
                engine_.run(cfg.max_cycles);
                retract_transient();
                outcome_.apply_guardrails(resp);
            }
            return resp;
        };
    }
};

} // namespace tiny_agent::middleware
```

Implementation caveat to verify while here: after `retract_fact`, rete's refraction memory (`agenda_.has_fired`) may keep rule+token pairs marked fired across runs, which would make a second identical event never fire. The "transient facts do not leak" test does not cover re-firing; ADD this test now:

```cpp
TEST_CASE("the same reflex fires again on a later identical event") {
    middleware::ReflexEngine rx;
    int fires = 0;
    rx.engine().add_rule("re")
        .when(std::string("msg"), std::string("text"), std::string("ping"))
        .then([&](rete::ReteEngine&, rete::Bindings&) { ++fires; rx.outcome().respond("pong"); })
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
```

If it fails because of refraction, call `engine_.clear_refraction()` at the start of each middleware run (that is what the API is for; new WME ids make each run's activations distinct anyway, so cross-run refraction state is pure leak).

- [ ] **Step 4: Run all reflex tests** — `ctest --test-dir build -R test_reflex --output-on-failure` — expected: PASS.

- [ ] **Step 5: Commit** — `git add include/tiny_agent/middleware/reflex.hpp tests/test_reflex.cpp && git commit -m "feat: reflex middleware — rete rules short-circuit the model"`

### Task 4: Guardrail path

**Files:**
- Modify: `tests/test_reflex.cpp` (append; implementation already landed in Task 3's header)

**Interfaces:**
- Consumes: `ReflexOutcome::veto/replace_arg/apply_guardrails`, `ReflexConfig::extract_response_facts` (Task 3).

- [ ] **Step 1: Write the failing tests**

```cpp
static LLMResponse model_calls_tool(const std::string& tool, json args) {
    Message m = Message::assistant("");
    m.tool_calls.push_back({"call-0", tool, std::move(args)});
    return {std::move(m), {}, "tool_calls", {}};
}

TEST_CASE("guardrail vetoes a forbidden tool call") {
    middleware::ReflexEngine rx;
    rx.engine().add_rule("no-delete")
        .when(std::string("?c"), std::string("tool"), std::string("delete_everything"))
        .then([&rx](rete::ReteEngine&, rete::Bindings&) {
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
        .then([&rx](rete::ReteEngine&, rete::Bindings&) {
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
        .then([&rx](rete::ReteEngine&, rete::Bindings&) { rx.outcome().veto(0, "blocked"); })
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
```

Note the `?c` variable in the veto rule: it matches any identifier, which is how one rule covers `call-0`, `call-1`, ... — but `veto(0, ...)` hardcodes index 0. For multi-call responses the rule action must derive the index from bindings; that refinement belongs to the Pip brain when it needs it, and gets a `// veto-by-binding: see Bindings docs` comment in the example, not speculative machinery here (YAGNI).

- [ ] **Step 2: Run** — first test may already pass if Task 3's implementation is complete; run all: `ctest --test-dir build -R test_reflex --output-on-failure`. Fix any failure inside `apply_guardrails` only.

- [ ] **Step 3: Commit** — `git add tests/test_reflex.cpp && git commit -m "test: guardrail veto, rewrite, and pass-through"`

### Task 5: Default fact extractors

**Files:**
- Modify: `include/tiny_agent/middleware/reflex.hpp` (append two free functions)
- Test: `tests/test_reflex.cpp` (append)

**Interfaces:**
- Produces: `middleware::message_facts(const std::vector<Message>&) -> std::vector<Fact>` and `middleware::tool_call_facts(const LLMResponse&) -> std::vector<Fact>`; Pip's brain and the example use these instead of hand-rolled lambdas.

- [ ] **Step 1: Write the failing tests**

```cpp
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
    // ("call-0","tool","led"), ("call-0","index",0), ("call-0","arg:r",255),
    // ("call-0","arg:nested","{\"x\":1}")
    REQUIRE(facts.size() == 4);
    CHECK(std::get<std::string>(facts[0][2]) == "led");
    CHECK(std::get<int64_t>(facts[1][2]) == 0);
}
```

- [ ] **Step 2: Run to verify failure**, then **Step 3: Implement** (append to reflex.hpp, same namespace):

```cpp
inline std::vector<Fact> message_facts(const std::vector<Message>& msgs) {
    std::vector<Fact> facts;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->role == Role::system) continue;
        facts.push_back({std::string("msg"), std::string("role"),
                         std::string(to_string(it->role))});
        facts.push_back({std::string("msg"), std::string("text"), it->text()});
        break;
    }
    return facts;
}

inline std::vector<Fact> tool_call_facts(const LLMResponse& resp) {
    std::vector<Fact> facts;
    for (size_t i = 0; i < resp.message.tool_calls.size(); ++i) {
        auto& tc = resp.message.tool_calls[i];
        std::string id = "call-" + std::to_string(i);
        facts.push_back({id, std::string("tool"), tc.name});
        facts.push_back({id, std::string("index"), static_cast<int64_t>(i)});
        if (tc.arguments.is_object())
            for (auto& [k, v] : tc.arguments.items())
                facts.push_back({id, "arg:" + k, integrations::to_rete_value(v)});
    }
    return facts;
}
```

Arg iteration order: nlohmann::json objects iterate in sorted key order, so the test's expected sequence (`arg:nested` before `arg:r`) must match sorted order — fix the test's index expectations to sorted keys if the first run disagrees, and pin the order in a comment.

- [ ] **Step 4: Run to verify pass**, then **Step 5: Commit** — `git commit -am "feat: default fact extractors for reflex middleware"`

### Task 6: rete_tool

**Files:**
- Create: `include/tiny_agent/tools/rete_tool.hpp`
- Test: `tests/test_reflex.cpp` (append)

**Interfaces:**
- Consumes: `DynamicTool` (core/tool.hpp), conversion helpers (Task 2).
- Produces: `tiny_agent::tools::rete_tool(ReteToolConfig) -> DynamicTool`; args `{"facts": [[id,attr,value],...]}`, returns `{"facts": [...]}` — all working-memory triples after the run. Fresh engine per invocation.

- [ ] **Step 1: Write the failing tests**

```cpp
#include <tiny_agent/tools/rete_tool.hpp>

TEST_CASE("rete_tool derives facts from a rulebase") {
    auto tool = tools::rete_tool({
        .name = "animal_expert",
        .description = "Classify animals from observed traits.",
        .setup = [](rete::ReteEngine& eng) {
            eng.add_rule("mammal")
                .when(std::string("?x"), std::string("has"), std::string("fur"))
                .then([&eng](rete::ReteEngine& e, rete::Bindings& b) {
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
```

`b.at("?x")`: confirm `Bindings`'s lookup API in `~/git/rete_cpp/include/rete/token.hpp` or `production.hpp` (it is the map produced by `Activation::compute_bindings()`); use its real accessor. If bindings are keyed without the `?` prefix, adjust both test and doc comment.

- [ ] **Step 2: Run to verify failure**, then **Step 3: Implement**:

```cpp
#pragma once
#include "../core/tool.hpp"
#include "../integrations/rete_convert.hpp"
#include <rete/rete.hpp>

namespace tiny_agent::tools {

struct ReteToolConfig {
    std::string name = "expert_system";
    std::string description = "Evaluate facts against a rule base and return derived facts.";
    std::function<void(rete::ReteEngine&)> setup;   // registers rules on a fresh engine
    int max_cycles = 256;
};

// A fresh engine per invocation keeps the tool deterministic and stateless;
// rule bases are cheap to rebuild at this scale.
inline DynamicTool rete_tool(ReteToolConfig cfg) {
    if (!cfg.setup) throw ToolError("rete_tool: setup is required");
    json params = {
        {"type", "object"},
        {"properties", {{"facts", {
            {"type", "array"},
            {"description", "Triples [id, attribute, value]"},
            {"items", {{"type", "array"}}}}}}},
        {"required", {"facts"}}};

    return DynamicTool::create(cfg.name, cfg.description,
        [cfg](const json& args) -> json {
            rete::ReteEngine eng;
            cfg.setup(eng);
            for (auto& f : args.value("facts", json::array())) {
                if (!f.is_array() || f.size() != 3)
                    throw ToolError("rete_tool: each fact must be [id, attr, value]");
                eng.assert_fact(integrations::to_rete_value(f[0]),
                                integrations::to_rete_value(f[1]),
                                integrations::to_rete_value(f[2]));
            }
            eng.run(cfg.max_cycles);
            json out = json::array();
            for (auto& w : eng.facts())
                out.push_back({integrations::from_rete_value(w->identifier),
                               integrations::from_rete_value(w->attribute),
                               integrations::from_rete_value(w->value)});
            return json{{"facts", out}};
        },
        std::move(params));
}

} // namespace tiny_agent::tools
```

- [ ] **Step 4: Run to verify pass**, then **Step 5: Commit** — `git add include/tiny_agent/tools/rete_tool.hpp tests/test_reflex.cpp && git commit -m "feat: rete_tool — a ruleset as a callable tool"`

### Task 7: Example, docs, CI

**Files:**
- Create: `examples/21_reflex_agent.cpp`
- Modify: `examples/CMakeLists.txt` (mirror how 20_context_management is added, guarded by the same rete option as tests)
- Modify: `docs/integrations.md` (one matrix row), `README.md` (replace the Plan-1 pointer line with a short section)
- Modify: `.github/workflows/` CI workflow (add `-DTINY_AGENT_FETCH_RETE=ON` to the cmake configure of the test job)

**Interfaces:**
- Consumes: everything above by its final names.

- [ ] **Step 1: Write the example** — self-contained, no API keys: build a `ReflexEngine` with the ping rule and the delete-veto rule, run a `MiddlewareChain` against a stub terminal standing in for a model, print both paths' outputs and the veto log. Structure and print style copied from `examples/17_streaming.cpp`'s header comment convention. Include the `// veto-by-binding` note from Task 4.

- [ ] **Step 2: Build and run it** — `cmake --build build --target 21_reflex_agent && ./build/examples/21_reflex_agent` — expected: prints the reflex answer, the model answer, and one veto entry; exits 0.

- [ ] **Step 3: Docs** — `docs/integrations.md` row: `| rete_cpp | reflex + guardrail middleware, expert-system tool | header-only, optional |`. README section (sweep with ai-tells.md):

```markdown
### Reflexes and guardrails (rete_cpp)

Pair tiny_agent with [rete_cpp](https://github.com/tinyagent-cc/rete_cpp) and
the middleware answers the easy cases before the model runs: a Rete rule that
matches returns in microseconds and spends zero tokens, and after the model
runs, guardrail rules veto or rewrite tool calls deterministically.

    middleware::ReflexEngine rx;
    rx.engine().add_rule("ping")
        .when("msg", "text", "ping")
        .then([&](auto&, auto&) { rx.outcome().respond("pong"); })
        .build();
    chain.add(rx.middleware({.extract_facts = middleware::message_facts}));

Build with `-DTINY_AGENT_RETE_DIR=/path/to/rete_cpp` (or
`-DTINY_AGENT_FETCH_RETE=ON`). See `examples/21_reflex_agent.cpp`.
```

Verify the string-literal `.when("msg", ...)` overload actually compiles (RuleBuilder takes `Value`; `const char*` must convert — if ambiguous, show `std::string("msg")` in the README instead; the README snippet is compiled by `test_readme_snippets.cpp` conventions, check how that test ingests snippets and register this one the same way if that is the house rule).

- [ ] **Step 4: CI** — add the flag to the test job's cmake configure; run `actionlint` if available. FetchContent needs network in CI, which the runners have.

- [ ] **Step 5: Full suite** — `ctest --test-dir build --output-on-failure` — expected: everything green including all reflex cases. Then commit: `git commit -am "docs+ci: reflex example, integrations row, CI coverage"`.

### Task 8: Merge

- [ ] **Step 1:** `git checkout main && git merge --no-ff feat/reflex-middleware`, run the full suite once more on main (both configures: with `TINY_AGENT_RETE_DIR` and vanilla), push. Delete the branch.

## Self-review notes

Spec coverage: pre-LLM reflex (T3), post-LLM guardrail (T4), tool adapter (T6), optional-header pattern + C++17-under-C++20 check (T1), tests host-side (T1-T6), integrations matrix + docs (T7). Dependency direction enforced structurally: only tiny_agent files include rete headers. Types cross-checked: `Fact`, `ReflexOutcome` method names, `message_facts`/`tool_call_facts`, `rete_tool` signature used consistently in T3-T7. Known verification points called out inline rather than assumed: `Action`/`Bindings` exact signature (T1/T6), refraction across runs (T3), json key order (T5), `const char*`→`Value` conversion (T7).
