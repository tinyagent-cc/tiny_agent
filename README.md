# tiny_agent

`tiny_agent` is a header-only C++20 agent framework for building LLM-powered applications with a small, composable API.

It includes:
- OpenAI, Anthropic, and Gemini providers
- local OpenAI-compatible providers such as Ollama, llama.cpp, and vLLM
- tool calling with JSON schemas
- multi-turn chat and conversation history
- middleware
- nested/sub-agent delegation
- MCP stdio integration
- multimodal messages

## Requirements

- CMake 3.20+
- A C++20 compiler
- [`vcpkg`](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set
- Internet access plus provider API keys for cloud-backed examples

## Dependencies

This repository uses a `vcpkg.json` manifest. Configuring the project installs:

- `nlohmann-json`
- `cpp-httplib` with OpenSSL support
- `doctest`
- `libenvpp`
- `json-schema-validator`

The core library target is header-only and primarily relies on `nlohmann-json` and `cpp-httplib`. The other dependencies are used by the repository's tests and validation helpers.

## Quick Start

From the repository root:

```bash
cmake --preset default
cmake --build --preset default
```

To build an optimized version:

```bash
cmake --preset release
cmake --build --preset release
```

### macOS / Linux / Raspberry Pi

```bash
export VCPKG_ROOT="$HOME/src/vcpkg"
export OPENAI_API_KEY="your-key-here"

cmake --preset default
cmake --build --preset default
./build/examples/01_basic_chat
```

### Windows PowerShell

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
$env:OPENAI_API_KEY = "your-key-here"

cmake --preset default
cmake --build --preset default --config Debug
.\build\examples\Debug\01_basic_chat.exe
```

On single-config generators the binary may instead be under `build/examples/01_basic_chat` or `build/examples/01_basic_chat.exe`.

## Provider Setup

Environment variable names used by the code:

- OpenAI: `OPENAI_API_KEY`
- Anthropic: `CLAUDE_API_KEY`
- Gemini: `GEMINI_API_KEY`

The example binaries read API keys from the shell environment.

The integration test in `tests/test_agent.cpp` also loads a repo-local `.env` file, so this works for the full test suite:

```dotenv
OPENAI_API_KEY=your-openai-key
CLAUDE_API_KEY=your-anthropic-key
GEMINI_API_KEY=your-gemini-key
```

## Basic Usage

### Minimal chat example

```cpp
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <cstdlib>
#include <iostream>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) {
        std::cerr << "OPENAI_API_KEY not set\n";
        return 1;
    }

    auto llm = LLM<openai>{"gpt-4o-mini", key};

    auto response = llm.chat({
        Message::system("You are a concise assistant."),
        Message::user("What is the capital of Japan?")
    });

    std::cout << response.message.text() << "\n";
}
```

### Agent with tools

```cpp
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>

int main() {
    using namespace tiny_agent;

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key) {
        std::cerr << "OPENAI_API_KEY not set\n";
        return 1;
    }

    auto agent = Agent{
        LLM<openai>{"gpt-4o-mini", key},
        AgentConfig{
            .system_prompt = "Use tools for calculations.",
            .tools = {
                Tool::create(
                    "sqrt",
                    "Square root of a number",
                    [](const json& args) -> json {
                        return std::sqrt(args["x"].get<double>());
                    },
                    {
                        {"type", "object"},
                        {"properties", {{"x", {{"type", "number"}}}}},
                        {"required", {"x"}}
                    }
                )
            }
        },
        Log{std::cerr, LogLevel::info}
    };

    std::cout << agent.run("What is sqrt(144)?") << "\n";
}
```

### Local / OpenAI-compatible providers

You can also target a local or self-hosted OpenAI-compatible endpoint:

```cpp
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/local.hpp>
#include <iostream>

int main() {
    using namespace tiny_agent;

    auto agent = Agent{
        local::ollama("llama3"),
        AgentConfig{
            .system_prompt = "Reply briefly."
        }
    };

    std::cout << agent.run("Give me one sentence about C++20.") << "\n";
}
```

Helpers available in `providers/local.hpp`:

- `tiny_agent::local::ollama()` with default base URL `http://localhost:11434`
- `tiny_agent::local::llamacpp()` with default base URL `http://localhost:8080`
- `tiny_agent::local::vllm()` with default base URL `http://localhost:8000`
- `tiny_agent::local::create()` for any other OpenAI-compatible endpoint

## Logging

The library uses a single `Log` class with six levels. The default level is `warn`, so everything is quiet out of the box. Lower the level to trace framework internals.

Available levels (from most to least verbose):

| Level | What it shows |
|-------|---------------|
| `trace` | Raw HTTP bodies, JSON-RPC messages, tool arguments/results, full message contents |
| `debug` | Agent loop iterations, LLM request/response summaries, token usage, MCP tool discovery |
| `info` | Tool calls, MCP connection events |
| `warn` | Max iterations reached, retry attempts |
| `error` | HTTP failures, tool execution errors, MCP errors |
| `off` | Silent |

### Agent logging

```cpp
auto agent = Agent{
    LLM<openai>{"gpt-4o-mini", key},
    AgentConfig{.name = "my_agent"},
    Log{std::cerr, LogLevel::debug}
};
```

Output at `debug` level:

```
[DEBUG] [my_agent] initializing (max_iterations=10 tools=1 middlewares=0)
[DEBUG] [my_agent] run("What is sqrt(144)?")
[DEBUG] [my_agent] iteration 1/10 (messages=2)
[DEBUG] [my_agent] LLM requested 1 tool call(s)
[INFO] [my_agent] calling tool: sqrt
[DEBUG] [my_agent] iteration 2/10 (messages=4)
[DEBUG] [my_agent] done: stop
```

### LLM logging

The `LLMConfig` has its own `Log` field for HTTP-level tracing:

```cpp
auto llm = LLM<openai>{"gpt-4o-mini", LLMConfig{
    .api_key = key,
    .log = Log{std::cerr, LogLevel::debug}
}};
```

At `debug` you see request summaries and token usage. At `trace` you see the full request/response JSON.

### MCP logging

```cpp
auto mcp = mcp::connect_stdio(command, args, Log{std::cerr, LogLevel::debug});
```

### Runtime changes

```cpp
agent.log().set_level(LogLevel::trace);
```

### Timestamps

```cpp
Log log{std::cerr, LogLevel::debug};
log.set_timestamps(true);
// 14:30:05.123 [DEBUG] [my_agent] iteration 1/10 (messages=2)
```

### Middleware logging

```cpp
middleware::logging(Log{std::cerr, LogLevel::debug})
```

## Building Your Own Program

The simplest way to use this project in another CMake build is to vendor it and add it as a subdirectory:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/tiny_agent_cpp)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE tiny_agent)
```

Then include the headers you need in your program:

```cpp
#include <tiny_agent/tiny_agent.hpp>
#include <tiny_agent/providers/openai.hpp>
```

This repository does not currently install/export a packaged CMake target, so `add_subdirectory(...)` is the easiest integration path.

## Build Options

The top-level CMake options are:

- `TINY_AGENT_BUILD_EXAMPLES=ON`
- `TINY_AGENT_BUILD_TESTS=ON`

For a library-only build:

```bash
cmake --preset default -DTINY_AGENT_BUILD_EXAMPLES=OFF -DTINY_AGENT_BUILD_TESTS=OFF
cmake --build --preset default
```

## Run Examples

The repository currently builds these examples:

- `01_basic_chat`: raw `LLM::chat(...)`
- `02_tool_calling`: function calling with JSON-schema-style parameters
- `03_nested_agents`: manager/researcher/writer composition
- `04_mcp_client`: MCP stdio client that exposes MCP tools to an agent
- `05_middleware`: logging, retry, trim-history, and custom middleware
- `06_deep_agent`: multi-level delegation with fact-checking
- `07_multimodal`: image + text message input

Most examples use `OPENAI_API_KEY`, so set that first.

Run them from the repository root:

```bash
./build/examples/01_basic_chat
./build/examples/02_tool_calling
./build/examples/03_nested_agents
./build/examples/05_middleware
./build/examples/06_deep_agent
./build/examples/07_multimodal
```

The MCP example takes the server command on the command line:

```bash
./build/examples/04_mcp_client npx @modelcontextprotocol/server-filesystem .
```

That example requires Node.js plus the MCP server package you want to launch.

## Run Tests

Run the full suite:

```bash
ctest --preset default
```

On Visual Studio or other multi-config generators:

```powershell
ctest --preset default -C Debug
```

The offline tests are:

- `test_types`
- `test_tool`
- `test_middleware`

The integration test is:

- `test_agent`

`test_agent` exercises real providers and expects API keys. It loads `.env` from the repository root.

## Platform Notes

### macOS

- Install Xcode Command Line Tools, CMake, and `vcpkg`.
- HTTPS requests use the system certificate bundle path `/etc/ssl/cert.pem` on Apple platforms. If you hit TLS errors, make sure that file exists and points to a valid CA bundle.

### Linux

- Any recent GCC or Clang with C++20 support should work.
- Install the usual build tools first, for example `build-essential`, `cmake`, `git`, `curl`, `zip`, `unzip`, and `tar`.

### Raspberry Pi

- Use a 64-bit Raspberry Pi OS image if possible.
- Follow the same Linux build steps.
- Prefer `cmake --preset release` for smaller, faster binaries on Pi hardware.
- Cloud providers generally work well on Raspberry Pi because the heavy inference runs remotely.
- For local inference, point `providers/local.hpp` at any OpenAI-compatible server reachable from the Pi, whether it runs on the Pi itself or elsewhere on your network.

### Windows

- Use Visual Studio 2022 Build Tools or a full Visual Studio install with the C++ workload.
- `cmake --build` and `ctest` may need `--config Debug` or `--config Release` with multi-config generators.
- PowerShell environment variables use the `$env:NAME = "value"` syntax shown above.

## Verified Commands

The following repository commands were verified successfully on macOS:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```
