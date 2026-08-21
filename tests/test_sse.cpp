#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/core/sse.hpp>
#include <string>
#include <vector>

using namespace tiny_agent;

// ── Helper: drain a parser over a set of chunks, collecting every event ──────

static std::vector<sse::Event> parse_all(const std::vector<std::string>& chunks,
                                         bool call_finish = true) {
    sse::Parser parser;
    std::vector<sse::Event> out;
    auto sink = [&](sse::Event e) { out.push_back(std::move(e)); };
    for (auto& c : chunks) parser.feed(c, sink);
    if (call_finish) parser.finish(sink);
    return out;
}

// ── Basic dispatch ───────────────────────────────────────────────────────────

TEST_CASE("sse: single complete event") {
    auto ev = parse_all({"data: hello\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "hello");
    CHECK(ev[0].event == "");
}

TEST_CASE("sse: leading space after colon is stripped once") {
    auto ev = parse_all({"data:  two spaces\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == " two spaces");
}

TEST_CASE("sse: value with no space after colon") {
    auto ev = parse_all({"data:nospace\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "nospace");
}

TEST_CASE("sse: named event with data") {
    auto ev = parse_all({"event: message_start\ndata: {\"x\":1}\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].event == "message_start");
    CHECK(ev[0].data == "{\"x\":1}");
}

// ── Multiple events / chunk boundaries ───────────────────────────────────────

TEST_CASE("sse: multiple events in one chunk") {
    auto ev = parse_all({"data: a\n\ndata: b\n\ndata: c\n\n"});
    REQUIRE(ev.size() == 3);
    CHECK(ev[0].data == "a");
    CHECK(ev[1].data == "b");
    CHECK(ev[2].data == "c");
}

TEST_CASE("sse: event split mid-data across chunks") {
    auto ev = parse_all({"data: hel", "lo wor", "ld\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "hello world");
}

TEST_CASE("sse: event boundary split across chunks") {
    // The blank-line terminator itself is split between chunks.
    auto ev = parse_all({"data: one\n", "\ndata: two\n\n"});
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].data == "one");
    CHECK(ev[1].data == "two");
}

TEST_CASE("sse: field name split across chunks") {
    auto ev = parse_all({"da", "ta: split-field\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "split-field");
}

// ── Line endings ─────────────────────────────────────────────────────────────

TEST_CASE("sse: CRLF line endings") {
    auto ev = parse_all({"data: crlf\r\n\r\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "crlf");
}

TEST_CASE("sse: CRLF split so CR and LF land in different chunks") {
    auto ev = parse_all({"data: x\r", "\n\r", "\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "x");
}

// ── Multi-line data ──────────────────────────────────────────────────────────

TEST_CASE("sse: multi-line data joined with newline") {
    auto ev = parse_all({"data: line1\ndata: line2\ndata: line3\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "line1\nline2\nline3");
}

// ── Comments ─────────────────────────────────────────────────────────────────

TEST_CASE("sse: comment lines are ignored") {
    auto ev = parse_all({": this is a comment\ndata: payload\n\n"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "payload");
}

TEST_CASE("sse: comment-only block dispatches nothing") {
    auto ev = parse_all({": keepalive\n\n"});
    CHECK(ev.empty());
}

// ── [DONE] passthrough ───────────────────────────────────────────────────────

TEST_CASE("sse: [DONE] is emitted like any other event") {
    auto ev = parse_all({"data: {\"k\":1}\n\ndata: [DONE]\n\n"});
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].data == "{\"k\":1}");
    CHECK(ev[1].data == "[DONE]");
}

// ── finish() flush ───────────────────────────────────────────────────────────

TEST_CASE("sse: trailing unterminated event is flushed by finish()") {
    auto ev = parse_all({"data: no trailing blank line"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "no trailing blank line");
}

TEST_CASE("sse: finish() flushes multi-line unterminated event") {
    auto ev = parse_all({"data: a\ndata: b"});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].data == "a\nb");
}

TEST_CASE("sse: without finish(), an unterminated event is not emitted") {
    auto ev = parse_all({"data: dangling"}, /*call_finish=*/false);
    CHECK(ev.empty());
}

TEST_CASE("sse: finish() on empty stream emits nothing") {
    auto ev = parse_all({});
    CHECK(ev.empty());
}
