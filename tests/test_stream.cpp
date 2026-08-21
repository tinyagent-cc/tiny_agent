#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/sse.hpp>
#include <tiny_agent/core/stream.hpp>
#include <tiny_agent/providers/openai.hpp>
#include <tiny_agent/providers/anthropic.hpp>
#include <string>
#include <vector>

using namespace tiny_agent;

// ── Test harness ─────────────────────────────────────────────────────────────
//
// Drives the exact production path the offline spec describes:
//   raw SSE bytes  ->  sse::Parser  ->  provider decoder  ->  StreamAccumulator
// and returns the folded LLMResponse (with tool-call ids restored) plus the
// text collected live from text_delta events.

template<typename Decoder>
struct DecodeResult {
    LLMResponse response;
    std::string streamed_text;
    std::vector<StreamEvent::Kind> kinds;
};

template<typename Decoder>
static DecodeResult<Decoder> decode_stream(const std::string& raw) {
    sse::Parser parser;
    Decoder decoder;
    StreamAccumulator acc;
    DecodeResult<Decoder> r;

    auto sink = [&](const StreamEvent& e) {
        acc.push(e);
        r.kinds.push_back(e.kind);
        if (e.kind == StreamEvent::Kind::text_delta) r.streamed_text += e.text;
    };
    auto on_sse = [&](sse::Event ev) { decoder(ev, sink); };

    parser.feed(raw, on_sse);
    parser.finish(on_sse);

    r.response = acc.result();
    decoder.restore_ids(r.response);
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  StreamAccumulator (pure, no provider)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("accumulator: folds text deltas into a message") {
    StreamAccumulator acc;
    acc.push({StreamEvent::Kind::message_start});
    acc.push({StreamEvent::Kind::text_delta, "Hello"});
    acc.push({StreamEvent::Kind::text_delta, ", world"});
    acc.push({StreamEvent::Kind::finish, "", -1, "", "", "stop"});

    auto r = acc.result();
    CHECK(r.message.role == Role::assistant);
    CHECK(r.message.text() == "Hello, world");
    CHECK(r.finish_reason == "stop");
    CHECK(r.message.tool_calls.empty());
}

TEST_CASE("accumulator: assembles a tool call from fragments") {
    StreamAccumulator acc;
    StreamEvent name; name.kind = StreamEvent::Kind::tool_call_delta;
    name.tool_index = 0; name.tool_name = "add";
    acc.push(name);

    StreamEvent a1; a1.kind = StreamEvent::Kind::tool_call_delta;
    a1.tool_index = 0; a1.tool_args = "{\"a\":17,";
    acc.push(a1);

    StreamEvent a2; a2.kind = StreamEvent::Kind::tool_call_delta;
    a2.tool_index = 0; a2.tool_args = "\"b\":25}";
    acc.push(a2);

    acc.push({StreamEvent::Kind::finish, "", -1, "", "", "tool_calls"});

    auto r = acc.result();
    REQUIRE(r.message.tool_calls.size() == 1);
    CHECK(r.message.tool_calls[0].name == "add");
    CHECK(r.message.tool_calls[0].arguments == json{{"a", 17}, {"b", 25}});
    CHECK(r.finish_reason == "tool_calls");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  OpenAI-compatible translation
// ═══════════════════════════════════════════════════════════════════════════════

static const std::string kOpenAIText =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"index\":0,\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"index\":0,\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\", world\"},\"index\":0,\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

static const std::string kOpenAITool =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":"
    "[{\"index\":0,\"id\":\"call_abc\",\"type\":\"function\",\"function\":{\"name\":\"add\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"a\\\":17,\"}}]},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"b\\\":25}\"}}]},\"finish_reason\":null}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";

TEST_CASE("openai stream: text matches the non-streaming response") {
    auto r = decode_stream<OpenAIStreamDecoder>(kOpenAIText);
    CHECK(r.streamed_text == "Hello, world");
    CHECK(r.response.message.text() == "Hello, world");
    CHECK(r.response.finish_reason == "stop");
    CHECK(r.response.message.tool_calls.empty());
}

TEST_CASE("openai stream: tool call matches the non-streaming response") {
    auto r = decode_stream<OpenAIStreamDecoder>(kOpenAITool);

    // Equivalent to what LLMModel<OpenAI, chat_tag>::chat() would return.
    REQUIRE(r.response.message.tool_calls.size() == 1);
    auto& tc = r.response.message.tool_calls[0];
    CHECK(tc.id == "call_abc");
    CHECK(tc.name == "add");
    CHECK(tc.arguments == json{{"a", 17}, {"b", 25}});
    CHECK(r.response.finish_reason == "tool_calls");
    CHECK(r.response.message.text() == "");
}

TEST_CASE("openai stream: survives chunk boundaries mid-token") {
    // Feed one byte at a time — the accumulated result must be identical.
    sse::Parser parser;
    OpenAIStreamDecoder decoder;
    StreamAccumulator acc;
    auto sink = [&](const StreamEvent& e) { acc.push(e); };
    auto on_sse = [&](sse::Event ev) { decoder(ev, sink); };
    for (char c : kOpenAIText) parser.feed(std::string_view(&c, 1), on_sse);
    parser.finish(on_sse);

    auto r = acc.result();
    CHECK(r.message.text() == "Hello, world");
    CHECK(r.finish_reason == "stop");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Anthropic translation
// ═══════════════════════════════════════════════════════════════════════════════

static const std::string kAnthropicText =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"role\":\"assistant\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\", world\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":5}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const std::string kAnthropicTool =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_2\",\"role\":\"assistant\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":20,\"output_tokens\":1}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"multiply\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"a\\\":6,\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"b\\\":7}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":10}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

TEST_CASE("anthropic stream: text matches the non-streaming response") {
    auto r = decode_stream<AnthropicStreamDecoder>(kAnthropicText);
    CHECK(r.streamed_text == "Hello, world");
    CHECK(r.response.message.text() == "Hello, world");
    CHECK(r.response.finish_reason == "end_turn");
    CHECK(r.response.message.tool_calls.empty());
}

TEST_CASE("anthropic stream: tool call matches the non-streaming response") {
    auto r = decode_stream<AnthropicStreamDecoder>(kAnthropicTool);
    REQUIRE(r.response.message.tool_calls.size() == 1);
    auto& tc = r.response.message.tool_calls[0];
    CHECK(tc.id == "toolu_1");
    CHECK(tc.name == "multiply");
    CHECK(tc.arguments == json{{"a", 6}, {"b", 7}});
    CHECK(r.response.finish_reason == "tool_use");
    CHECK(r.response.message.text() == "");
}
