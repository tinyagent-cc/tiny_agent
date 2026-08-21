# Real streaming for tiny_agent (v0.3, M1)

Replaces the fake `stream()` (single callback with the full response, model.hpp:188, agent.hpp:219) with true SSE token streaming. Design goal, per Riadh: decoupled components, each testable alone, no provider forced to participate.

## Components (each its own header, no cross-dependencies except types)

### 1. `core/sse.hpp` — SSE parser
A standalone incremental parser. Feed it raw transport chunks, it emits complete SSE events. It knows nothing about JSON, providers, or HTTP.

```cpp
namespace tiny_agent::sse {
struct Event { std::string event; std::string data; };
class Parser {
public:
    // Feed a raw chunk; invokes sink for each completed event.
    void feed(std::string_view chunk, const std::function<void(Event)>& sink);
    void finish(const std::function<void(Event)>& sink); // flush trailing event
};
}
```
Handles: events split mid-line across chunks, multiple events per chunk, `\n` and `\r\n`, multi-line `data:` fields, comment lines, `[DONE]` passthrough (emitted like any event; interpretation is the caller's job).

### 2. `core/stream.hpp` — stream event model
Provider-agnostic event type. Providers translate their wire format into this; consumers see only this.

```cpp
namespace tiny_agent {
struct StreamEvent {
    enum class Kind { message_start, text_delta, tool_call_delta, finish };
    Kind kind;
    std::string text;          // text_delta: the token(s)
    int tool_index = -1;       // tool_call_delta: which call
    std::string tool_name;     // tool_call_delta: name fragment (may be empty)
    std::string tool_args;     // tool_call_delta: arguments JSON fragment
    std::string finish_reason; // finish
};
using StreamHandler = std::function<void(const StreamEvent&)>;
}
```
Also here: `StreamAccumulator`, which folds a sequence of StreamEvents into a final `LLMResponse` (message text, assembled tool calls, finish reason). Pure, testable without any network.

### 3. Provider capability: `chat_stream`
OpenAI-compatible and Anthropic specializations gain:

```cpp
LLMResponse chat_stream(const std::vector<Message>& msgs,
                        const std::vector<ToolSchema>& tools,
                        const StreamHandler& on_event);
```
Implementation: `stream: true` in the request body, cpp-httplib `ContentReceiver` feeding `sse::Parser`, provider-specific translation of each SSE event into `StreamEvent`s pushed to both the handler and a `StreamAccumulator`; returns the accumulated `LLMResponse`. So every streaming call also yields the complete response, and the agent loop can consume streaming and non-streaming providers identically. Gemini's wire format differs; it is out of scope for M1 and must not block this design (that is the point of the decoupling).

### 4. Concept: `is_streaming_chat` (optional refinement, core/model.hpp)
```cpp
template<typename T>
concept is_streaming_chat = is_chat<T> && requires(T m, ...) {
    { m.chat_stream(msgs, tools, handler) } -> std::same_as<LLMResponse>;
};
```
Nothing in `is_chat` changes; providers without streaming still satisfy it. `ChatVariant::stream` and `AgentExecutor::stream` use `if constexpr (is_streaming_chat<...>)` to stream when the underlying model can, and fall back to today's invoke-then-callback-once otherwise. No virtual dispatch, consistent with the codebase.

### 5. `AgentExecutor` surface (agent.hpp)
```cpp
std::string run_stream(const std::string& input, const StreamHandler& on_event);
std::string chat_stream(const std::string& input, const StreamHandler& on_event); // multi-turn
```
The ReAct loop calls `chat_stream` on the model when available: text deltas and tool-call deltas flow to the handler live across all iterations (so a UI can show the agent thinking and calling tools), and the loop logic itself is unchanged because each streamed call still returns a full `LLMResponse`. The existing `stream(std::string, std::function<void(std::string)>)` is reimplemented on top (text_delta events only) and kept for compatibility.

## Testing (doctest, no network)
- `test_sse.cpp`: chunk-boundary torture tests — event split mid-`data:`, multi-event chunk, CRLF, comments, trailing unterminated event via `finish()`.
- `test_stream.cpp`: canned OpenAI and Anthropic SSE fixture strings, fed through Parser + provider translation + StreamAccumulator, asserting the final LLMResponse equals the non-streaming equivalent (text, tool calls, finish reason). Fixtures live in the test file as raw strings.
- Existing live integration tests keep working; a live streaming smoke test goes behind the same env-key guard pattern as test_agent.cpp.

## Definition of done
- All existing 188 tests still pass; new tests pass.
- `examples/01_basic_chat` gains a streaming variant printing tokens as they arrive against Ollama (OpenAI-compatible path) — this is the demo-critical path for M2.
- No public API breaks: `is_chat`, `chat()`, `run()`, existing `stream()` signatures unchanged.
