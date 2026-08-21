# tiny_agent

[![CI](https://github.com/rhajamor/tiny_agent/actions/workflows/ci.yml/badge.svg)](https://github.com/rhajamor/tiny_agent/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**The header-only C++20 agent framework: multi-provider, MCP built in, MIT licensed, and small enough to run your agent on a Raspberry Pi.**

Inference in C++ is solved. llama.cpp and Ollama run the model, and llama.cpp's
own docs say the agent loop is the caller's problem. That loop is what
tiny_agent is: the delegation, the middleware, the tools, the memory, in a few
thousand lines of headers with no virtual dispatch anywhere in the dispatch
path. Point it at a frontier API or at a llama.cpp server on localhost and the
agent code does not change.

Three things carry the library.

## Sub-agents that are just tools

`DeepAgent` runs a ReAct loop: call the model, dispatch the tools it asked for,
feed the results back, repeat. What makes it a *deep* agent is that an agent can
become a tool for another agent, so delegation needs no new concept. A director
calls an analyst; the analyst calls a fact-checker; each one is a `DynamicTool`
with a JSON schema, and each can run on a different model from a different
provider.

```cpp
auto fact_checker = make_shared_agent(
    OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
    AgentConfig{.name = "fact_checker",
                .system_prompt = "Reply VERIFIED or UNVERIFIED with one line of why."});

auto analyst = make_shared_agent(
    AnthropicChat{.model = "claude-sonnet-4-5", .api_key = anthropic_key},
    AgentConfig{
        .name = "analyst",
        .system_prompt = "Analyze the topic. Verify every claim with fact_checker.",
        .tools = {agent_as_tool(fact_checker, "fact_checker", "Verify a claim")}});

auto director = make_shared_agent(
    OpenAIChat{.model = "gpt-4o", .api_key = key},
    AgentConfig{
        .name = "director",
        .tools = {agent_as_tool(analyst, "analyst", "Analyze a topic")},
        .logger = Log{std::cerr, LogLevel::debug}});

std::cout << director->run("What did renewables do to global CO2 emissions?");
```

A tool that throws never takes down the loop: the exception becomes a
`tool_result` the model reads and recovers from, whether it derives from
`std::exception` or not. A model that emits truncated tool arguments gets the
same treatment rather than an exception out of the JSON parser. Delegation runs
through `shared_ptr`, and calling `as_tool()` on a stack-allocated agent tells
you which constructor to use instead of throwing `std::bad_weak_ptr` from inside
the standard library.

Examples: [`06_deep_agent.cpp`](examples/06_deep_agent.cpp),
[`16_deep_agent_custom.cpp`](examples/16_deep_agent_custom.cpp).

## Middleware that wraps the model call

One signature. A middleware gets the mutable message vector and a `Next` it must
call to reach the model:

```cpp
using MiddlewareFn = std::function<LLMResponse(std::vector<Message>&, Next)>;
```

That is enough to rewrite the prompt, short-circuit without calling the model,
retry by calling `next` twice, or fall back to another provider. Fourteen are
built in:

| | |
|---|---|
| Context | `context_management`, `summarize`, `trim_history`, `context_editing` |
| Reliability | `retry`, `model_retry`, `model_fallback`, `model_call_limit`, `tool_call_limit` |
| Observability | `tracing`, `logging` |
| Content | `pii`, `system_prompt`, `rationalize` |

```cpp
AgentConfig cfg;
cfg.middlewares = {
    middleware::logging(Log{std::cerr, LogLevel::debug}),
    middleware::tracing({.tracer = tracer}),
    middleware::context_management({.budget = {.max_tokens = 8000, .keep_recent = 6}}),
    middleware::model_retry({.max_retries = 3}),
};
```

There is a compile-time chain too. `make_middleware_stack(...)` folds the same
callables through a `std::tuple` with no `std::function` and no allocation, for
when the stack is known at build time.

Examples: [`05_middleware.cpp`](examples/05_middleware.cpp),
[`examples/middleware/`](examples/middleware) (eleven, one per middleware).

## Providers behind concepts, not interfaces

There is no `IChatModel`. A provider is a full specialization of
`LLMModel<Provider, Kind>` that satisfies a concept:

```cpp
template<typename T>
concept is_chat =
    std::same_as<typename T::model_tag, chat_tag> &&
    requires(T m, const std::vector<Message>& msgs, const std::vector<ToolSchema>& tools) {
        { m.chat(msgs, tools) } -> std::same_as<LLMResponse>;
        { m.model_name()      } -> std::convertible_to<std::string>;
    };
```

Agents are templates over that concept, so the model call is a direct call with
no vtable and no type erasure. Streaming is a separate refinement,
`is_streaming_chat`, which providers opt into; the agent branches on
`if constexpr` and providers without it still satisfy `is_chat`.

Seven providers ship: OpenAI, Anthropic, Gemini, Mistral, Cohere, VoyageAI, and
any OpenAI-compatible endpoint through `local::ollama()`, `local::llamacpp()`
and `local::vllm()`. Adding one is a header, not a change to the core.

When the provider comes from a config file rather than the type system,
`ChatVariant` dispatches over `std::variant` and `init_chat_model("openai:gpt-4o")`
builds one at runtime. `Runnable` composes models, agents and plain lambdas with
`operator|`.

## What else is in the box

- **MCP** over stdio and HTTP, no paid tier
- **[Agent Skills](https://agentskills.io)** (`SKILL.md`) loading, which no other C++ framework does
- **True SSE streaming**, tool-call deltas included, on the OpenAI-compatible and Anthropic paths
- **[Tracing](docs/observability.md)** to Arize Phoenix, Langfuse, or any OTLP collector, with no vendor SDK
- **[Vector stores](docs/vector-stores.md)**: in-process Flat, hnswlib, Qdrant, Chroma, Weaviate, Redis, Milvus, behind one four-method concept
- **[Context management](docs/context-management.md)** against an explicit token budget
- Multimodal messages, embeddings, batch, an agent-skills registry

## Footprint

Measured, not estimated (details in [docs/benchmarks.md](docs/benchmarks.md)): a
complete streaming agent example is a **7.7 MB** stripped single binary (macOS
arm64, TLS included), and the client uses **2.0 MB RSS** while streaming from
llama.cpp on a Raspberry Pi 5. The agent layer is not the cost; the model is. CI
builds and tests every push on arm64, the same CPU class as a Pi 5.

Both are the real `17_streaming` example, over SSH, against a local llama.cpp
server:

| Raspberry Pi 5 | Jetson Orin Nano |
|---|---|
| ![17_streaming on a Raspberry Pi 5](docs/assets/pi5-streaming.gif) | ![17_streaming on a Jetson Orin Nano](docs/assets/jetson-streaming.gif) |
| Qwen2.5-3B-Instruct, CPU only: 5.48 tok/s | Qwen2.5-3B-Instruct, GPU, 36 layers offloaded: 23.85 tok/s |

How that compares in the C++ agent space, structurally:

| | tiny_agent | [ai-sdk-cpp](https://github.com/ClickHouse/ai-sdk-cpp) | [agents.cpp](https://github.com/RunEdgeAI/agents.cpp) |
|---|---|---|---|
| Header-only | yes | no | no |
| License | MIT | Apache-2.0 | evaluation license, MCP in paid tier |
| MCP | stdio + HTTP, free | no | paid |
| Local models (Ollama/llama.cpp) | yes | OpenAI-compatible only | yes |
| Gemini | yes | no | yes |
| Build | CMake + vcpkg | CMake | Bazel |
| Agent Skills (SKILL.md) | yes | no | no |

## Quick start

You need CMake 3.20+, a C++20 compiler, and [`vcpkg`](https://github.com/microsoft/vcpkg)
with `VCPKG_ROOT` set. Configuring installs `nlohmann-json`, `cpp-httplib` with
OpenSSL, `doctest`, `libenvpp` and `json-schema-validator`. The library target
itself only needs the first two; the rest are for tests.

```bash
export VCPKG_ROOT="$HOME/src/vcpkg"
export OPENAI_API_KEY="your-key-here"

cmake --preset default          # or --preset release
cmake --build --preset default
./build/examples/01_basic_chat
```

On Windows PowerShell the same commands work with `$env:VCPKG_ROOT = "C:\src\vcpkg"`
and `--config Debug` on the build and test steps.

### Chat

```cpp
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>

auto llm = OpenAIChat{.model = "gpt-4o-mini", .api_key = key};

auto response = llm.chat({Message::system("Be concise."),
                          Message::user("What is the capital of Japan?")});
std::cout << response.message.text() << "\n";
```

### An agent with a tool

```cpp
auto agent = make_agent(
    OpenAIChat{.model = "gpt-4o-mini", .api_key = key},
    AgentConfig{
        .name = "math_agent",
        .system_prompt = "Use tools for calculations.",
        .tools = {
            DynamicTool::create("sqrt", "Square root of a number",
                [](const json& p) -> json { return std::sqrt(p["x"].get<double>()); },
                {{"type", "object"},
                 {"properties", {{"x", {{"type", "number"}}}}},
                 {"required", {"x"}}})
        },
        .logger = Log{std::cerr, LogLevel::info}});

std::cout << agent.run("What is sqrt(144)?") << "\n";
```

### Against a local model

```cpp
#include <tiny_agent/providers/local.hpp>

auto agent = make_agent(local::ollama("llama3"),
                        AgentConfig{.system_prompt = "Reply briefly."});
std::cout << agent.run("One sentence about C++20.") << "\n";
```

`local::ollama()` defaults to `http://localhost:11434`, `local::llamacpp()` to
port 8080, `local::vllm()` to port 8000, and `local::create()` takes any other
OpenAI-compatible URL.

## Logging

One `Log` class, six levels, default `warn`, so the library is quiet out of the
box.

| Level | What it shows |
|-------|---------------|
| `trace` | Raw HTTP bodies, JSON-RPC messages, tool arguments and results, full message contents |
| `debug` | Loop iterations, request and response summaries, token usage, MCP tool discovery |
| `info` | Tool calls, MCP connection events |
| `warn` | Max iterations reached, retry attempts, context still over budget |
| `error` | HTTP failures, tool execution errors, exporter failures |
| `off` | Silent |

```cpp
auto agent = make_agent(llm, AgentConfig{.name = "my_agent",
                                         .logger = Log{std::cerr, LogLevel::debug}});
```

```
[DEBUG] [my_agent] initializing (max_iterations=10 tools=1 middlewares=0)
[DEBUG] [my_agent] run iteration 1/10 (messages=2)
[DEBUG] [my_agent] LLM requested 1 tool call(s)
[INFO] [my_agent] calling tool: sqrt
[DEBUG] [my_agent] run iteration 2/10 (messages=4)
[DEBUG] [my_agent] done: stop
```

`LLMConfig` carries its own `Log` for HTTP-level tracing, `mcp::connect_stdio()`
takes one, `middleware::logging()` is one, and `agent.log().set_level(...)`
changes it at runtime. `log.set_timestamps(true)` prefixes each line.

## Provider setup

`OPENAI_API_KEY`, `CLAUDE_API_KEY`, `GEMINI_API_KEY`. Examples read them from the
environment. `tests/test_agent.cpp` also loads a repo-root `.env`.

## Using it in your own build

Vendor it and add a subdirectory:

```cmake
add_subdirectory(external/tiny_agent_cpp)
target_link_libraries(my_app PRIVATE tiny_agent)
```

Build options: `TINY_AGENT_BUILD_EXAMPLES`, `TINY_AGENT_BUILD_TESTS`,
`TINY_AGENT_BUILD_BENCH` (all `ON`), and `TINY_AGENT_HNSWLIB` (`OFF`) for the
hnswlib vector store. Set these `OFF` for a vendored build; a consumer does not
need the examples, tests, or benchmarks.

Or install it and `find_package`:

```bash
cmake --preset default -DTINY_AGENT_BUILD_EXAMPLES=OFF -DTINY_AGENT_BUILD_TESTS=OFF -DTINY_AGENT_BUILD_BENCH=OFF
cmake --build --preset default
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(tiny_agent CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE tiny_agent::tiny_agent)
```

Point CMake at the prefix with `-DCMAKE_PREFIX_PATH=/path/to/prefix` if it is
not a system location.

**vcpkg, before this port is in the public registry.** `ports/tiny-agent` in
this repo is a working port; use it as an overlay until it upstreams:

```bash
vcpkg install tiny-agent --overlay-ports=/path/to/tiny_agent_cpp/ports/tiny-agent
```

or in a manifest project, add an `"overlay-ports"` entry to
`vcpkg-configuration.json` pointing at that directory and depend on
`tiny-agent` from `vcpkg.json` as usual.

## Examples

Twenty numbered examples plus eleven middleware ones.

| | |
|---|---|
| `01`–`03` | chat, tool calling, nested agents |
| `04`, `12` | MCP over stdio and HTTP |
| `05`, `10` | middleware, custom and built-in |
| `06`, `16` | deep agents and delegation |
| `07`, `08` | multimodal, batch |
| `09`, `14` | runtime provider selection, bind and kwargs |
| `11` | Agent Skills |
| `13`, `19` | embeddings and retrieval, vector store backends |
| `15`, `20` | LLM summarization, context management |
| `17` | SSE streaming against Ollama or llama.cpp |
| `18` | tracing to Phoenix, Langfuse or stderr |

Most need `OPENAI_API_KEY`. `18`, `19` and `20` run without one. The MCP example
takes the server command on the command line and needs Node:

```bash
./build/examples/04_mcp_client npx @modelcontextprotocol/server-filesystem .
```

## Tests

```bash
ctest --preset default          # add -C Debug on multi-config generators
```

383 offline doctest cases across 22 files, no network and no keys required.
`test_agent` is the one file on top of those; it calls real providers, needs API
keys, and is the only one that will fail without them.

Tests that need a service skip themselves unless it is configured:

```bash
PHOENIX_BASE_URL=http://localhost:6006 ctest --preset default -R test_tracing
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
  ctest --preset default -R test_vectorstore_remote
WEAVIATE_URL=http://localhost:8080 ctest --preset default -R test_vs_weaviate
REDIS_URL=redis://localhost:6379 ctest --preset default -R test_vs_redis
MILVUS_URL=http://localhost:19530 ctest --preset default -R test_vs_milvus
```

## Docs

- [Observability](docs/observability.md): tracing, exporters, Phoenix, Langfuse
- [Vector stores](docs/vector-stores.md): the store concept, Qdrant, Chroma, Weaviate, Redis, Milvus
- [Integration status](docs/integrations.md): live-verified backends versus offline-only, per integration
- [Context management](docs/context-management.md): token budgets and compaction
- [Benchmarks](docs/benchmarks.md): binary size, RSS, time to first token
- [Direction](docs/direction-2026-08.md): where this is going and why

## Platform notes

- **macOS**: HTTPS uses `/etc/ssl/cert.pem`. TLS errors usually mean that file is missing.
- **Linux**: any recent GCC or Clang with C++20. Install `build-essential`, `cmake`, `git`, `curl`, `zip`, `unzip`, `tar`.
- **Raspberry Pi**: 64-bit Raspberry Pi OS, the Linux steps, and `--preset release`. Cloud providers work fine because the inference happens elsewhere; for local inference point `local::llamacpp()` at any OpenAI-compatible server on the Pi or on your network.
- **Windows**: Visual Studio 2022 Build Tools with the C++ workload. The MCP stdio transport is POSIX-only, so `04_mcp_client` is not built there.

## License

MIT. See [LICENSE](LICENSE).
