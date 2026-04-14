#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tiny_agent/memory/store.hpp>
#include <tiny_agent/memory/cache.hpp>

using namespace tiny_agent;
using namespace tiny_agent::memory;

// ═══════════════════════════════════════════════════════════════════════════
// InMemoryStore — basic operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("InMemoryStore: put and get") {
    InMemoryStore<8> store;
    store.put("key1", "value1");
    auto val = store.get("key1");
    REQUIRE(val.has_value());
    CHECK(*val == "value1");
}

TEST_CASE("InMemoryStore: get returns nullopt for missing key") {
    InMemoryStore<8> store;
    CHECK(!store.get("missing").has_value());
}

TEST_CASE("InMemoryStore: has") {
    InMemoryStore<8> store;
    CHECK(!store.has("key1"));
    store.put("key1", "v");
    CHECK(store.has("key1"));
}

TEST_CASE("InMemoryStore: update existing key") {
    InMemoryStore<8> store;
    store.put("key1", "old");
    store.put("key1", "new");
    CHECK(*store.get("key1") == "new");
    CHECK(store.size() == 1);
}

TEST_CASE("InMemoryStore: remove") {
    InMemoryStore<8> store;
    store.put("key1", "v");
    store.remove("key1");
    CHECK(!store.has("key1"));
    CHECK(store.size() == 0);
}

TEST_CASE("InMemoryStore: remove non-existent is safe") {
    InMemoryStore<8> store;
    store.remove("nope");  // should not throw
    CHECK(store.size() == 0);
}

TEST_CASE("InMemoryStore: clear") {
    InMemoryStore<8> store;
    store.put("a", "1");
    store.put("b", "2");
    store.clear();
    CHECK(store.size() == 0);
    CHECK(!store.has("a"));
}

TEST_CASE("InMemoryStore: size tracking") {
    InMemoryStore<8> store;
    CHECK(store.size() == 0);
    store.put("a", "1");
    CHECK(store.size() == 1);
    store.put("b", "2");
    CHECK(store.size() == 2);
    store.remove("a");
    CHECK(store.size() == 1);
}

TEST_CASE("InMemoryStore: capacity constant") {
    CHECK(InMemoryStore<64>::capacity() == 64);
    CHECK(InMemoryStore<1>::capacity() == 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// InMemoryStore — LRU eviction
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("InMemoryStore: evicts LRU when full") {
    InMemoryStore<3> store;
    store.put("a", "1");
    store.put("b", "2");
    store.put("c", "3");
    CHECK(store.size() == 3);

    store.put("d", "4");  // evicts "a" (oldest)
    CHECK(store.size() == 3);
    CHECK(!store.has("a"));
    CHECK(store.has("b"));
    CHECK(store.has("d"));
}

TEST_CASE("InMemoryStore: get promotes to MRU") {
    InMemoryStore<3> store;
    store.put("a", "1");
    store.put("b", "2");
    store.put("c", "3");

    store.get("a");       // promote "a" → now "b" is LRU
    store.put("d", "4");  // evicts "b"

    CHECK(store.has("a"));
    CHECK(!store.has("b"));
    CHECK(store.has("c"));
    CHECK(store.has("d"));
}

TEST_CASE("InMemoryStore: put existing promotes to MRU") {
    InMemoryStore<3> store;
    store.put("a", "1");
    store.put("b", "2");
    store.put("c", "3");

    store.put("a", "updated");  // promote "a" → "b" is LRU
    store.put("d", "4");        // evicts "b"

    CHECK(*store.get("a") == "updated");
    CHECK(!store.has("b"));
}

TEST_CASE("InMemoryStore: capacity 1") {
    InMemoryStore<1> store;
    store.put("a", "1");
    CHECK(*store.get("a") == "1");
    store.put("b", "2");
    CHECK(!store.has("a"));
    CHECK(*store.get("b") == "2");
}

// ═══════════════════════════════════════════════════════════════════════════
// ToolCache
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("ToolCache: store and lookup") {
    ToolCache<InMemoryStore<16>> cache;
    json args = {{"query", "test"}};

    cache.store("search", args, "result data");
    auto hit = cache.lookup("search", args);
    REQUIRE(hit.has_value());
    CHECK(*hit == "result data");
}

TEST_CASE("ToolCache: miss returns nullopt") {
    ToolCache<InMemoryStore<16>> cache;
    CHECK(!cache.lookup("search", {{"q", "x"}}).has_value());
}

TEST_CASE("ToolCache: different args are different keys") {
    ToolCache<InMemoryStore<16>> cache;
    cache.store("search", {{"q", "a"}}, "result_a");
    cache.store("search", {{"q", "b"}}, "result_b");

    CHECK(*cache.lookup("search", {{"q", "a"}}) == "result_a");
    CHECK(*cache.lookup("search", {{"q", "b"}}) == "result_b");
}

TEST_CASE("ToolCache: different tools same args") {
    ToolCache<InMemoryStore<16>> cache;
    cache.store("search", {{"q", "x"}}, "search_result");
    cache.store("fetch",  {{"q", "x"}}, "fetch_result");

    CHECK(*cache.lookup("search", {{"q", "x"}}) == "search_result");
    CHECK(*cache.lookup("fetch",  {{"q", "x"}}) == "fetch_result");
}

TEST_CASE("ToolCache: has") {
    ToolCache<InMemoryStore<16>> cache;
    json args = {{"x", 1}};
    CHECK(!cache.has("tool", args));
    cache.store("tool", args, "val");
    CHECK(cache.has("tool", args));
}

TEST_CASE("ToolCache: invalidate") {
    ToolCache<InMemoryStore<16>> cache;
    json args = {{"x", 1}};
    cache.store("tool", args, "val");
    cache.invalidate("tool", args);
    CHECK(!cache.has("tool", args));
}

TEST_CASE("ToolCache: clear") {
    ToolCache<InMemoryStore<16>> cache;
    cache.store("a", {}, "1");
    cache.store("b", {}, "2");
    cache.clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("ToolCache: eviction via underlying store") {
    ToolCache<InMemoryStore<2>> cache;
    cache.store("a", {}, "1");
    cache.store("b", {}, "2");
    cache.store("c", {}, "3");  // evicts "a"
    CHECK(!cache.lookup("a", {}).has_value());
    CHECK(cache.lookup("c", {}).has_value());
    CHECK(cache.size() == 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// cached() — tool wrapper
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cached: returns same result on repeated calls") {
    int call_count = 0;
    auto tool = Tool::create("add", "adds numbers",
        [&](const json& args) -> json {
            ++call_count;
            return args["a"].get<int>() + args["b"].get<int>();
        });

    auto cached_tool = memory::cached<32>(std::move(tool));

    json args = {{"a", 2}, {"b", 3}};
    auto r1 = cached_tool(args);
    auto r2 = cached_tool(args);

    CHECK(r1 == 5);
    CHECK(r2 == 5);
    CHECK(call_count == 1);  // only called once — second was cache hit
}

TEST_CASE("cached: different args call through") {
    int call_count = 0;
    auto tool = Tool::create("mul", "multiplies",
        [&](const json& args) -> json {
            ++call_count;
            return args["a"].get<int>() * args["b"].get<int>();
        });

    auto cached_tool = memory::cached<32>(std::move(tool));

    CHECK(cached_tool({{"a", 2}, {"b", 3}}) == 6);
    CHECK(cached_tool({{"a", 4}, {"b", 5}}) == 20);
    CHECK(call_count == 2);
}

TEST_CASE("cached: preserves tool schema") {
    auto tool = Tool::create("my_tool", "desc",
        [](const json&) -> json { return "ok"; },
        {{"type", "object"}, {"properties", {{"x", {{"type", "string"}}}}}});

    auto cached_tool = memory::cached<8>(std::move(tool));
    CHECK(cached_tool.schema.name == "my_tool");
    CHECK(cached_tool.schema.description == "desc");
    CHECK(cached_tool.schema.parameters["properties"]["x"]["type"] == "string");
}

TEST_CASE("cached: string results cached correctly") {
    int calls = 0;
    auto tool = Tool::create("echo", "echoes",
        [&](const json& args) -> json {
            ++calls;
            return args["text"].get<std::string>();
        });

    auto cached_tool = memory::cached<8>(std::move(tool));

    json args = {{"text", "hello"}};
    auto r1 = cached_tool(args);
    auto r2 = cached_tool(args);
    CHECK(r1 == "hello");
    CHECK(r2 == "hello");
    CHECK(calls == 1);
}

TEST_CASE("cached: shared cache across tools") {
    auto shared = std::make_shared<ToolCache<InMemoryStore<32>>>();

    int a_calls = 0, b_calls = 0;
    auto tool_a = Tool::create("a", "tool a",
        [&](const json& args) -> json { ++a_calls; return "a:" + args.dump(); });
    auto tool_b = Tool::create("b", "tool b",
        [&](const json& args) -> json { ++b_calls; return "b:" + args.dump(); });

    auto cached_a = memory::cached(std::move(tool_a), shared);
    auto cached_b = memory::cached(std::move(tool_b), shared);

    json args = {{"x", 1}};
    cached_a(args);
    cached_a(args);  // cache hit
    cached_b(args);
    cached_b(args);  // cache hit

    CHECK(a_calls == 1);
    CHECK(b_calls == 1);
    CHECK(shared->size() == 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// memory_store concept satisfaction
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("InMemoryStore satisfies memory_store concept") {
    static_assert(memory_store<InMemoryStore<1>>);
    static_assert(memory_store<InMemoryStore<64>>);
    static_assert(memory_store<InMemoryStore<1024>>);
}
