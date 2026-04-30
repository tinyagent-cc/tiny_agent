#include <tiny_agent/core/types.hpp>
#include <tiny_agent/core/tool.hpp>
#include <tiny_agent/core/runnable.hpp>
#include <tiny_agent/core/middleware.hpp>
#include <tiny_agent/middleware/all.hpp>
#include <tiny_agent/core/log.hpp>
#include <tiny_agent/agent.hpp>
#include <tiny_agent/memory/store.hpp>
#include <tiny_agent/memory/cache.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace tiny_agent;
using Clock = std::chrono::high_resolution_clock;

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark infrastructure
// ═══════════════════════════════════════════════════════════════════════════════

struct BenchResult {
    std::string name;
    std::size_t iterations;
    double min_ns, max_ns, mean_ns, median_ns, stddev_ns;
    double ops_per_sec;
    std::size_t peak_rss_kb = 0;
};

static std::vector<double> run_samples(std::function<void()> fn, int warmup, int n) {
    for (int i = 0; i < warmup; ++i) fn();

    std::vector<double> samples(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        samples[static_cast<std::size_t>(i)] =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
    }
    return samples;
}

static BenchResult bench(const std::string& name, std::function<void()> fn,
                         int warmup = 100, int iters = 10000) {
    auto samples = run_samples(fn, warmup, iters);
    std::sort(samples.begin(), samples.end());

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    double mean = sum / static_cast<double>(iters);
    double median = samples[samples.size() / 2];

    double sq_sum = 0;
    for (auto s : samples) sq_sum += (s - mean) * (s - mean);
    double stddev = std::sqrt(sq_sum / static_cast<double>(iters));

    return {name, static_cast<std::size_t>(iters),
            samples.front(), samples.back(), mean, median, stddev,
            1e9 / mean};
}

static std::string format_ns(double ns) {
    if (ns < 1000.0)       return std::to_string(static_cast<int>(ns)) + " ns";
    if (ns < 1'000'000.0)  { std::ostringstream o; o << std::fixed << std::setprecision(1) << ns/1000.0 << " us"; return o.str(); }
    if (ns < 1e9)           { std::ostringstream o; o << std::fixed << std::setprecision(2) << ns/1e6 << " ms"; return o.str(); }
    std::ostringstream o; o << std::fixed << std::setprecision(2) << ns/1e9 << " s"; return o.str();
}

static std::string format_ops(double ops) {
    if (ops >= 1e6) { std::ostringstream o; o << std::fixed << std::setprecision(1) << ops/1e6 << "M"; return o.str(); }
    if (ops >= 1e3) { std::ostringstream o; o << std::fixed << std::setprecision(1) << ops/1e3 << "K"; return o.str(); }
    std::ostringstream o; o << std::fixed << std::setprecision(0) << ops; return o.str();
}

static void print_results(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << std::left << std::setw(48) << "Benchmark"
              << std::right << std::setw(12) << "Median"
              << std::setw(12) << "Mean"
              << std::setw(12) << "Min"
              << std::setw(12) << "Stddev"
              << std::setw(14) << "Ops/sec"
              << "\n";
    std::cout << std::string(110, '-') << "\n";

    for (auto& r : results) {
        std::cout << std::left << std::setw(48) << r.name
                  << std::right << std::setw(12) << format_ns(r.median_ns)
                  << std::setw(12) << format_ns(r.mean_ns)
                  << std::setw(12) << format_ns(r.min_ns)
                  << std::setw(12) << format_ns(r.stddev_ns)
                  << std::setw(14) << format_ops(r.ops_per_sec)
                  << "\n";
    }
    std::cout << std::string(110, '=') << "\n\n";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Mock LLM — satisfies llm_like, returns canned responses in-process
// ═══════════════════════════════════════════════════════════════════════════════

struct MockLLM {
    using input_t   = std::string;
    using output_t  = std::string;
    using model_tag = chat_tag;

    std::string model = "mock-7b";
    int call_count = 0;
    int tool_calls_to_emit = 0;

    std::string invoke(const std::string&, const RunConfig& = {}) {
        return "The answer is 42.";
    }

    LLMResponse chat(const std::vector<Message>&,
                     const std::vector<ToolSchema>& tools = {}) {
        ++call_count;

        if (tool_calls_to_emit > 0 && !tools.empty() && call_count == 1) {
            LLMResponse resp{Message::assistant(""), {}, "tool_calls", {}};
            for (int i = 0; i < tool_calls_to_emit; ++i) {
                auto& t = tools[static_cast<std::size_t>(i) % tools.size()];
                resp.message.tool_calls.push_back({
                    "call_" + std::to_string(i),
                    t.name,
                    json{{"x", 42}, {"y", 7}}
                });
            }
            return resp;
        }
        return {Message::assistant("The answer is 42."),
                json{{"prompt_tokens", 50}, {"completion_tokens", 10}},
                "stop", {}};
    }

    std::string model_name() const { return model; }
    float temperature() const { return 0.7f; }

    std::vector<std::string> batch(std::vector<std::string> inputs, const RunConfig& cfg = {}) {
        std::vector<std::string> out;
        for (auto& in : inputs) out.push_back(invoke(in, cfg));
        return out;
    }
    void stream(std::string input, std::function<void(std::string)> cb, const RunConfig& cfg = {}) {
        cb(invoke(input, cfg));
    }
};

static_assert(is_chat<MockLLM>);

// ═══════════════════════════════════════════════════════════════════════════════
// Helper: generate realistic conversation histories
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<Message> make_history(std::size_t n, bool with_tools = false) {
    std::vector<Message> msgs;
    msgs.reserve(n + 1);
    msgs.push_back(Message::system("You are a helpful assistant for embedded systems."));
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 3 == 0) {
            msgs.push_back(Message::user("Turn " + std::to_string(i) +
                ": Read sensor data from GPIO pin " + std::to_string(i % 40)));
        } else if (i % 3 == 1 && with_tools) {
            auto m = Message::tool_result("tc_" + std::to_string(i),
                R"({"temperature": 23.5, "humidity": 67.2, "pressure": 1013.25, )"
                R"("timestamp": "2026-04-13T10:00:00Z", "status": "nominal"})");
            m.name = "read_sensor";
            msgs.push_back(std::move(m));
        } else {
            msgs.push_back(Message::assistant(
                "Sensor reading complete. Temperature is within normal range for the MCU. "
                "Humidity levels indicate no condensation risk for the PCB."));
        }
    }
    return msgs;
}

static std::string make_large_text(std::size_t approx_tokens) {
    std::string text;
    text.reserve(approx_tokens * 4);
    for (std::size_t i = 0; i < approx_tokens; ++i)
        text += "word ";
    return text;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Tool Registry
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<BenchResult> bench_tool_registry() {
    std::vector<BenchResult> results;
    std::cout << "=== TOOL REGISTRY ===\n";

    auto make_tool = [](const std::string& name) {
        return DynamicTool::create(name, "desc for " + name,
            [](const json& args) -> json {
                return json{{"result", args.value("x", 0) + args.value("y", 0)}};
            },
            json{{"type", "object"},
                 {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}}},
                 {"required", {"x", "y"}}});
    };

    // Tool lookup in registries of different sizes
    for (int n : {5, 20, 50, 100}) {
        ToolRegistry reg;
        for (int i = 0; i < n; ++i) reg.add(make_tool("tool_" + std::to_string(i)));

        results.push_back(bench("tool_lookup (" + std::to_string(n) + " tools)", [&] {
            volatile auto& t = reg.get("tool_" + std::to_string(n / 2));
            (void)t;
        }));
    }

    // Tool execution
    ToolRegistry reg;
    reg.add(make_tool("calc"));
    json args = {{"x", 100}, {"y", 200}};
    results.push_back(bench("tool_execute (arithmetic)", [&] {
        auto r = reg.execute("calc", args);
        (void)r;
    }));

    // DynamicTool::create overhead
    results.push_back(bench("tool_create", [&] {
        auto t = make_tool("dynamic_tool");
        (void)t;
    }));

    // Schema generation
    for (int n : {5, 20, 50}) {
        ToolRegistry r;
        for (int i = 0; i < n; ++i) r.add(make_tool("tool_" + std::to_string(i)));
        results.push_back(bench("schemas() (" + std::to_string(n) + " tools)", [&] {
            auto s = r.schemas();
            (void)s;
        }));
    }

    // Realistic tool: JSON parsing + string processing
    auto json_tool = DynamicTool::create("parse_sensor", "Parse sensor payload",
        [](const json& args) -> json {
            auto data = args.value("payload", "{}");
            auto parsed = json::parse(data);
            return json{{"temp", parsed.value("temperature", 0.0)},
                        {"status", "processed"}};
        });
    ToolRegistry reg2;
    reg2.add(json_tool);
    json sensor_args = {{"payload", R"({"temperature": 23.5, "humidity": 67.2, "pressure": 1013.25})"}};
    results.push_back(bench("tool_execute (JSON parse sensor)", [&] {
        auto r = reg2.execute("parse_sensor", sensor_args);
        (void)r;
    }));

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Middleware Pipeline
// ═══════════════════════════════════════════════════════════════════════════════

static LLMResponse terminal_passthrough(std::vector<Message>&) {
    return {Message::assistant("ok"), {}, "stop", {}};
}

static std::vector<BenchResult> bench_middleware() {
    std::vector<BenchResult> results;
    std::cout << "=== MIDDLEWARE PIPELINE ===\n";

    auto noop_mw = [](std::vector<Message>& msgs, Next next) -> LLMResponse {
        return next(msgs);
    };

    // Empty chain baseline
    {
        MiddlewareChain chain;
        auto msgs = make_history(10);
        results.push_back(bench("chain_empty (baseline)", [&] {
            auto r = chain.run(msgs, terminal_passthrough);
            (void)r;
        }));
    }

    // Runtime chain: varying depth
    for (int depth : {1, 3, 5, 10, 20}) {
        MiddlewareChain chain;
        for (int i = 0; i < depth; ++i) chain.add(noop_mw);
        auto msgs = make_history(10);
        results.push_back(bench("chain_runtime (depth=" + std::to_string(depth) + ")",
            [&] { auto r = chain.run(msgs, terminal_passthrough); (void)r; }));
    }

    // Static (compile-time) chain comparison
    {
        struct Noop {
            LLMResponse operator()(std::vector<Message>& msgs, Next next) const {
                return next(msgs);
            }
        };

        auto msgs = make_history(10);

        auto stack1 = StaticMiddlewareStack{Noop{}};
        results.push_back(bench("chain_static (depth=1)", [&] {
            auto r = stack1.run(msgs, terminal_passthrough); (void)r;
        }));

        auto stack3 = StaticMiddlewareStack{Noop{}, Noop{}, Noop{}};
        results.push_back(bench("chain_static (depth=3)", [&] {
            auto r = stack3.run(msgs, terminal_passthrough); (void)r;
        }));

        auto stack5 = StaticMiddlewareStack{Noop{}, Noop{}, Noop{}, Noop{}, Noop{}};
        results.push_back(bench("chain_static (depth=5)", [&] {
            auto r = stack5.run(msgs, terminal_passthrough); (void)r;
        }));

        auto stack10 = StaticMiddlewareStack{
            Noop{}, Noop{}, Noop{}, Noop{}, Noop{},
            Noop{}, Noop{}, Noop{}, Noop{}, Noop{}};
        results.push_back(bench("chain_static (depth=10)", [&] {
            auto r = stack10.run(msgs, terminal_passthrough); (void)r;
        }));
    }

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Individual Built-in Middleware
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<BenchResult> bench_builtin_middleware() {
    std::vector<BenchResult> results;
    std::cout << "=== BUILT-IN MIDDLEWARE ===\n";

    // SystemPrompt
    {
        middleware::SystemPrompt mw{"You are a helpful IoT assistant."};
        auto msgs = make_history(10);
        results.push_back(bench("SystemPrompt (inject)", [&] {
            auto m = std::vector<Message>{Message::user("hi")};
            auto r = mw(m, terminal_passthrough);
            (void)r;
        }));
        results.push_back(bench("SystemPrompt (already present)", [&] {
            auto r = mw(msgs, terminal_passthrough);
            (void)r;
        }));
    }

    // TrimHistory with varying conversation sizes
    for (std::size_t history_size : {20, 50, 100, 200}) {
        middleware::TrimHistory<10> mw;
        results.push_back(bench(
            "TrimHistory<10> (" + std::to_string(history_size) + " msgs)", [&] {
                auto msgs = make_history(history_size);
                auto r = mw(msgs, terminal_passthrough);
                (void)r;
            }, 50, 5000));
    }

    // Logging (to /dev/null equivalent)
    {
        std::ostringstream devnull;
        middleware::Logging mw{Log{devnull, LogLevel::off}};
        auto msgs = make_history(20);
        results.push_back(bench("Logging (off)", [&] {
            auto r = mw(msgs, terminal_passthrough);
            (void)r;
        }));

        middleware::Logging mw_on{Log{devnull, LogLevel::debug}};
        results.push_back(bench("Logging (debug, sink=null)", [&] {
            auto r = mw_on(msgs, terminal_passthrough);
            (void)r;
        }));
    }

    // PII detection (email)
    {
        auto mw = middleware::pii({.pii_type = "email", .strategy = "redact"});
        auto msgs_clean = std::vector<Message>{Message::user("Hello, how are you?")};
        auto msgs_pii = std::vector<Message>{
            Message::user("Contact me at user@example.com and admin@iot-device.local")};

        results.push_back(bench("PII (email, no match)", [&] {
            auto m = msgs_clean;
            auto r = mw(m, terminal_passthrough);
            (void)r;
        }));
        results.push_back(bench("PII (email, 2 matches)", [&] {
            auto m = msgs_pii;
            auto r = mw(m, terminal_passthrough);
            (void)r;
        }));
    }

    // ModelCallLimit
    {
        auto mw = middleware::model_call_limit({.limit = 100000});
        auto msgs = make_history(5);
        results.push_back(bench("ModelCallLimit (under limit)", [&] {
            auto r = mw(msgs, terminal_passthrough);
            (void)r;
        }));
    }

    // ToolCallLimit
    {
        auto mw = middleware::tool_call_limit({.limit = 100000});
        auto msgs = make_history(5);
        results.push_back(bench("ToolCallLimit (under limit)", [&] {
            auto r = mw(msgs, [](auto&) {
                LLMResponse resp{Message::assistant("ok"), {}, "stop", {}};
                resp.message.tool_calls.push_back({"tc1", "read_sensor", json::object()});
                return resp;
            });
            (void)r;
        }));
    }

    // Summarize (extractive)
    for (std::size_t n : {20, 50, 100}) {
        middleware::Summarize<100, 4> mw;
        results.push_back(bench(
            "Summarize<100,4> (" + std::to_string(n) + " msgs)", [&] {
                auto msgs = make_history(n, true);
                auto r = mw(msgs, terminal_passthrough);
                (void)r;
            }, 50, 5000));
    }

    // Rationalize
    {
        middleware::Rationalize<500> mw;
        auto msgs = make_history(10, true);
        // Add large tool result to trigger guidance
        msgs.push_back(Message::tool_result("big", make_large_text(1000)));
        results.push_back(bench("Rationalize (with large result)", [&] {
            auto m = msgs;
            auto r = mw(m, terminal_passthrough);
            (void)r;
        }, 50, 5000));

        auto small_msgs = make_history(10);
        results.push_back(bench("Rationalize (no large results)", [&] {
            auto m = small_msgs;
            auto r = mw(m, terminal_passthrough);
            (void)r;
        }));
    }

    // ContextEditing
    {
        auto mw = middleware::context_editing({.trigger = 50, .keep = 2});
        auto msgs = make_history(30, true);
        results.push_back(bench("ContextEditing (30 msgs, trigger=50)", [&] {
            auto m = msgs;
            auto r = mw(m, terminal_passthrough);
            (void)r;
        }, 50, 5000));
    }

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Memory Store & Cache
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<BenchResult> bench_memory() {
    std::vector<BenchResult> results;
    std::cout << "=== MEMORY STORE & CACHE ===\n";

    // InMemoryStore: put/get at different capacities
    for (std::size_t cap : {32, 128, 512}) {
        if (cap == 32) {
            memory::InMemoryStore<32> store;
            for (std::size_t i = 0; i < 30; ++i)
                store.put("key_" + std::to_string(i), "val_" + std::to_string(i));

            results.push_back(bench("LRU<32> get (hit)", [&] {
                auto v = store.get("key_15");
                (void)v;
            }));
            results.push_back(bench("LRU<32> get (miss)", [&] {
                auto v = store.get("nonexistent_key");
                (void)v;
            }));
            results.push_back(bench("LRU<32> put (update)", [&] {
                store.put("key_15", "updated");
            }));
            results.push_back(bench("LRU<32> put (evict)", [&] {
                store.put("evict_key", "evict_val");
            }));
        } else if (cap == 128) {
            memory::InMemoryStore<128> store;
            for (std::size_t i = 0; i < 120; ++i)
                store.put("key_" + std::to_string(i), "val_" + std::to_string(i));

            results.push_back(bench("LRU<128> get (hit)", [&] {
                auto v = store.get("key_60");
                (void)v;
            }));
            results.push_back(bench("LRU<128> put (evict)", [&] {
                store.put("evict_key", "evict_val");
            }));
        } else {
            memory::InMemoryStore<512> store;
            for (std::size_t i = 0; i < 500; ++i)
                store.put("key_" + std::to_string(i), "val_" + std::to_string(i));

            results.push_back(bench("LRU<512> get (hit)", [&] {
                auto v = store.get("key_250");
                (void)v;
            }));
            results.push_back(bench("LRU<512> put (evict)", [&] {
                store.put("evict_key", "evict_val");
            }));
        }
    }

    // ToolCache
    {
        memory::ToolCache<memory::InMemoryStore<256>> cache;
        json args = {{"sensor", "temp"}, {"pin", 4}};

        cache.store("read_sensor", args, R"({"value": 23.5})");

        results.push_back(bench("ToolCache lookup (hit)", [&] {
            auto v = cache.lookup("read_sensor", args);
            (void)v;
        }));
        results.push_back(bench("ToolCache lookup (miss)", [&] {
            json miss_args = {{"sensor", "pressure"}, {"pin", 7}};
            auto v = cache.lookup("read_sensor", miss_args);
            (void)v;
        }));
        results.push_back(bench("ToolCache store", [&] {
            cache.store("write_gpio", json{{"pin", 17}, {"val", 1}}, "ok");
        }));
    }

    // Cached tool wrapper
    {
        int call_count = 0;
        auto raw_tool = DynamicTool::create("compute", "heavy compute",
            [&](const json& args) -> json {
                ++call_count;
                return json{{"result", args.value("x", 0) * args.value("x", 0)}};
            });
        auto cached_tool = memory::cached<128>(std::move(raw_tool));
        json args = {{"x", 7}};

        cached_tool.fn(args); // prime cache
        results.push_back(bench("cached_tool (cache hit)", [&] {
            auto r = cached_tool.fn(args);
            (void)r;
        }));

        auto uncached = DynamicTool::create("compute2", "uncached",
            [](const json& args) -> json {
                return json{{"result", args.value("x", 0) * args.value("x", 0)}};
            });
        results.push_back(bench("uncached_tool (same work)", [&] {
            auto r = uncached.fn(args);
            (void)r;
        }));
    }

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Agent Loop (with MockLLM)
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<BenchResult> bench_agent_loop() {
    std::vector<BenchResult> results;
    std::cout << "=== AGENT LOOP (MockLLM) ===\n";

    auto make_math_tool = [](const std::string& name, auto op) {
        return DynamicTool::create(name, name + " two numbers",
            [op](const json& args) -> json {
                return json{{"result", op(args.value("x", 0), args.value("y", 0))}};
            },
            json{{"type", "object"},
                 {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}}},
                 {"required", {"x", "y"}}});
    };

    // Simple run: no tools, no middleware
    {
        MockLLM llm;
        AgentExecutor agent{std::move(llm), AgentConfig{.name = "bare"}};
        results.push_back(bench("agent.run (no tools, no mw)", [&] {
            auto r = agent.run("What is 2+2?");
            (void)r;
        }, 50, 5000));
    }

    // Run with tools (1 tool call round-trip)
    {
        MockLLM llm;
        llm.tool_calls_to_emit = 1;
        auto tool = make_math_tool("add", [](int a, int b) { return a + b; });
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "single_tool",
            .tools = {tool},
            .max_iterations = 5}};
        results.push_back(bench("agent.run (1 tool call)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Add 42 and 7");
            (void)r;
        }, 50, 5000));
    }

    // Run with multiple tools
    {
        MockLLM llm;
        llm.tool_calls_to_emit = 3;
        auto tools = std::vector<DynamicTool>{
            make_math_tool("add", [](int a, int b) { return a + b; }),
            make_math_tool("mul", [](int a, int b) { return a * b; }),
            make_math_tool("sub", [](int a, int b) { return a - b; }),
        };
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "multi_tool",
            .tools = tools,
            .max_iterations = 5}};
        results.push_back(bench("agent.run (3 parallel tool calls)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Compute add, mul, sub of 42 and 7");
            (void)r;
        }, 50, 5000));
    }

    // Run with middleware stack
    {
        MockLLM llm;
        std::ostringstream devnull;
        auto tools = std::vector<DynamicTool>{
            make_math_tool("add", [](int a, int b) { return a + b; }),
        };
        auto mws = std::vector<MiddlewareFn>{
            middleware::system_prompt("You are an IoT math assistant."),
            middleware::trim_history(20),
            middleware::logging(Log{devnull, LogLevel::off}),
        };
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "mw_agent",
            .tools = tools,
            .middlewares = mws,
            .max_iterations = 5}};
        results.push_back(bench("agent.run (3 mw + 1 tool)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Add 42 and 7");
            (void)r;
        }, 50, 5000));
    }

    // Full realistic stack: all middleware + multiple tools
    {
        MockLLM llm;
        llm.tool_calls_to_emit = 2;
        std::ostringstream devnull;
        auto tools = std::vector<DynamicTool>{
            make_math_tool("add", [](int a, int b) { return a + b; }),
            make_math_tool("mul", [](int a, int b) { return a * b; }),
            DynamicTool::create("read_sensor", "Read IoT sensor",
                [](const json& args) -> json {
                    return json{{"temp", 23.5 + args.value("pin", 0) * 0.1},
                                {"humidity", 67.2}};
                },
                json{{"type", "object"},
                     {"properties", {{"pin", {{"type", "number"}}}}},
                     {"required", {"pin"}}}),
        };
        auto mws = std::vector<MiddlewareFn>{
            middleware::system_prompt("You are an embedded systems assistant."),
            middleware::logging(Log{devnull, LogLevel::off}),
            middleware::trim_history(30),
            middleware::pii({.pii_type = "email", .strategy = "redact"}),
            middleware::model_call_limit({.limit = 100}),
            middleware::tool_call_limit({.limit = 50}),
        };
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "full_stack",
            .tools = tools,
            .middlewares = mws,
            .max_iterations = 10}};
        results.push_back(bench("agent.run (FULL: 6 mw + 3 tools + 2 calls)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Read sensor on pin 4 and compute area");
            (void)r;
        }, 50, 3000));
    }

    // Chat: multi-turn conversation
    {
        MockLLM llm;
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "chat_agent",
            .system_prompt = "IoT chat assistant"}};
        results.push_back(bench("agent.chat (10 turns)", [&] {
            agent.clear_history();
            for (int i = 0; i < 10; ++i)
                agent.chat("Turn " + std::to_string(i));
        }, 10, 1000));
    }

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Message & JSON Overhead
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<BenchResult> bench_message_overhead() {
    std::vector<BenchResult> results;
    std::cout << "=== MESSAGE & JSON OVERHEAD ===\n";

    results.push_back(bench("Message::user (short)", [&] {
        auto m = Message::user("hello");
        (void)m;
    }));

    results.push_back(bench("Message::user (256 chars)", [&] {
        auto m = Message::user(std::string(256, 'x'));
        (void)m;
    }));

    results.push_back(bench("Message::system (1KB)", [&] {
        auto m = Message::system(std::string(1024, 'x'));
        (void)m;
    }));

    results.push_back(bench("Message::tool_result (JSON 512B)", [&] {
        auto m = Message::tool_result("tc_1",
            R"({"readings": [1.2, 3.4, 5.6, 7.8, 9.0], "status": "ok", "device": "rpi-4b", "uptime": 86400})");
        (void)m;
    }));

    // Vector<Message> copy (simulates history management)
    auto history = make_history(50);
    results.push_back(bench("copy 50-msg history", [&] {
        auto copy = history;
        (void)copy;
    }, 50, 5000));

    auto big_history = make_history(200, true);
    results.push_back(bench("copy 200-msg history (w/tools)", [&] {
        auto copy = big_history;
        (void)copy;
    }, 20, 2000));

    // json::parse (realistic sensor payload)
    std::string payload = R"({
        "device_id": "rpi-001",
        "sensors": [
            {"name": "temp", "value": 23.5, "unit": "C"},
            {"name": "humidity", "value": 67.2, "unit": "%"},
            {"name": "pressure", "value": 1013.25, "unit": "hPa"}
        ],
        "gpio": {"pin_4": true, "pin_17": false, "pin_27": true},
        "timestamp": "2026-04-13T10:00:00Z"
    })";
    results.push_back(bench("json::parse (sensor payload ~300B)", [&] {
        auto j = json::parse(payload);
        (void)j;
    }));

    // json::dump
    auto j = json::parse(payload);
    results.push_back(bench("json::dump (sensor payload)", [&] {
        auto s = j.dump();
        (void)s;
    }));

    // ToolSchema creation
    results.push_back(bench("ToolSchema create (full params)", [&] {
        ToolSchema ts{"read_sensor", "Read data from sensor",
            json{{"type", "object"},
                 {"properties", {
                     {"pin", {{"type", "integer"}, {"description", "GPIO pin number"}}},
                     {"rate", {{"type", "number"}, {"description", "Sample rate in Hz"}}},
                     {"count", {{"type", "integer"}, {"description", "Number of samples"}}}
                 }},
                 {"required", {"pin"}}}};
        (void)ts;
    }));

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Benchmark: Constrained Environment Scenarios
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<BenchResult> bench_constrained_scenarios() {
    std::vector<BenchResult> results;
    std::cout << "=== CONSTRAINED ENVIRONMENT SCENARIOS ===\n";
    std::cout << "(Simulating Raspberry Pi / Jetson Nano workloads)\n\n";

    // Scenario 1: GPIO monitoring agent — frequent short tool calls
    {
        MockLLM llm;
        llm.tool_calls_to_emit = 1;
        auto gpio_tool = DynamicTool::create("read_gpio", "Read GPIO pin state",
            [](const json& args) -> json {
                auto pin = args.value("pin", 0);
                return json{{"pin", pin}, {"state", pin % 2 == 0}, {"voltage", 3.3}};
            },
            json{{"type", "object"},
                 {"properties", {{"pin", {{"type", "integer"}}}}},
                 {"required", {"pin"}}});

        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "gpio_monitor",
            .system_prompt = "Monitor GPIO pins. Report state changes.",
            .tools = {gpio_tool},
            .max_iterations = 3}};

        results.push_back(bench("GPIO monitor (1 read cycle)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Read pin 4");
            (void)r;
        }, 50, 5000));
    }

    // Scenario 2: Sensor aggregation — batch tool calls
    {
        MockLLM llm;
        llm.tool_calls_to_emit = 5;
        std::vector<DynamicTool> sensor_tools;
        for (auto name : {"temp", "humidity", "pressure", "light", "motion"}) {
            sensor_tools.push_back(DynamicTool::create(
                std::string("read_") + name, std::string("Read ") + name + " sensor",
                [](const json&) -> json {
                    return json{{"value", 23.5}, {"unit", "C"}, {"timestamp", 1234567890}};
                }));
        }
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "sensor_agg",
            .system_prompt = "Aggregate all sensor readings.",
            .tools = sensor_tools,
            .max_iterations = 3}};

        results.push_back(bench("Sensor aggregation (5 sensors)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Read all sensors");
            (void)r;
        }, 50, 3000));
    }

    // Scenario 3: Long-running chat with history management
    {
        MockLLM llm;
        std::ostringstream devnull;
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "chat_managed",
            .system_prompt = "IoT device assistant.",
            .middlewares = {
                middleware::trim_history(15),
                middleware::logging(Log{devnull, LogLevel::off}),
            },
            .max_iterations = 3}};

        results.push_back(bench("Managed chat (30 turns, trim=15)", [&] {
            agent.clear_history();
            for (int i = 0; i < 30; ++i)
                agent.chat("Sensor reading " + std::to_string(i));
        }, 5, 500));
    }

    // Scenario 4: PII-safe device logging
    {
        MockLLM llm;
        auto mws = std::vector<MiddlewareFn>{
            middleware::pii({.pii_type = "email", .strategy = "redact"}),
            middleware::pii({.pii_type = "ip", .strategy = "redact"}),
            middleware::pii({.pii_type = "phone", .strategy = "redact"}),
        };
        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "pii_safe",
            .middlewares = mws,
            .max_iterations = 2}};

        results.push_back(bench("PII-safe agent (3 PII filters)", [&] {
            auto r = agent.run("Device 192.168.1.1 owned by user@corp.com, phone +1-555-123-4567");
            (void)r;
        }, 50, 3000));
    }

    // Scenario 5: Full embedded stack (production-like)
    {
        MockLLM llm;
        llm.tool_calls_to_emit = 2;
        std::ostringstream devnull;

        auto tools = std::vector<DynamicTool>{
            DynamicTool::create("read_sensor", "Read sensor",
                [](const json& args) -> json {
                    return json{{"temp", 23.5 + args.value("pin", 0) * 0.1}};
                }),
            DynamicTool::create("write_gpio", "Write GPIO",
                [](const json& args) -> json {
                    return json{{"ok", true}, {"pin", args.value("pin", 0)}};
                }),
            DynamicTool::create("system_info", "Get system info",
                [](const json&) -> json {
                    return json{{"cpu_temp", 55.2}, {"mem_free_mb", 412},
                                {"uptime_s", 86400}, {"load", 0.35}};
                }),
        };
        auto mws = std::vector<MiddlewareFn>{
            middleware::system_prompt("Embedded systems assistant for Raspberry Pi 4B."),
            middleware::trim_history(20),
            middleware::logging(Log{devnull, LogLevel::off}),
            middleware::pii({.pii_type = "ip", .strategy = "redact"}),
            middleware::model_call_limit({.limit = 50}),
            middleware::tool_call_limit({.limit = 30}),
        };

        AgentExecutor agent{std::move(llm), AgentConfig{
            .name = "embedded_prod",
            .tools = tools,
            .middlewares = mws,
            .max_iterations = 5}};

        results.push_back(bench("EMBEDDED PROD (6 mw + 3 tools)", [&] {
            agent.llm().call_count = 0;
            auto r = agent.run("Check system health and read temp sensor on pin 4");
            (void)r;
        }, 50, 3000));
    }

    print_results(results);
    return results;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║     tiny_agent_cpp Benchmark Suite                           ║\n";
    std::cout << "║     Target: Constrained Environments (RPi, Jetson, Arduino)  ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n\n";

    std::vector<std::vector<BenchResult>> all;

    all.push_back(bench_message_overhead());
    all.push_back(bench_tool_registry());
    all.push_back(bench_middleware());
    all.push_back(bench_builtin_middleware());
    all.push_back(bench_memory());
    all.push_back(bench_agent_loop());
    all.push_back(bench_constrained_scenarios());

    // Summary: key numbers for constrained environments
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  SUMMARY — Key Numbers for Constrained Environments          ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n\n";

    auto find = [&](const std::string& name) -> const BenchResult* {
        for (auto& group : all)
            for (auto& r : group)
                if (r.name == name) return &r;
        return nullptr;
    };

    auto print_key = [](const char* label, const BenchResult* r) {
        if (!r) return;
        std::cout << "  " << std::left << std::setw(50) << label
                  << std::right << std::setw(10) << format_ns(r->median_ns)
                  << "  (" << format_ops(r->ops_per_sec) << " ops/s)\n";
    };

    print_key("Tool lookup (20 tools)",      find("tool_lookup (20 tools)"));
    print_key("Tool execute (arithmetic)",    find("tool_execute (arithmetic)"));
    print_key("Middleware chain (5 deep)",     find("chain_runtime (depth=5)"));
    print_key("Static chain (5 deep)",        find("chain_static (depth=5)"));
    print_key("LRU<128> get (cache hit)",     find("LRU<128> get (hit)"));
    print_key("Cached tool (hit)",            find("cached_tool (cache hit)"));
    print_key("Agent run (no tools)",          find("agent.run (no tools, no mw)"));
    print_key("Agent run (full stack)",        find("agent.run (FULL: 6 mw + 3 tools + 2 calls)"));
    print_key("GPIO monitor cycle",            find("GPIO monitor (1 read cycle)"));
    print_key("Embedded production agent",     find("EMBEDDED PROD (6 mw + 3 tools)"));
    print_key("json::parse (sensor payload)",  find("json::parse (sensor payload ~300B)"));

    // Static vs Runtime comparison
    auto rt5  = find("chain_runtime (depth=5)");
    auto st5  = find("chain_static (depth=5)");
    if (rt5 && st5 && st5->median_ns > 0) {
        double speedup = rt5->median_ns / st5->median_ns;
        std::cout << "\n  Static vs Runtime middleware (depth=5): "
                  << std::fixed << std::setprecision(1) << speedup << "x speedup\n";
    }
    auto rt10 = find("chain_runtime (depth=10)");
    auto st10 = find("chain_static (depth=10)");
    if (rt10 && st10 && st10->median_ns > 0) {
        double speedup = rt10->median_ns / st10->median_ns;
        std::cout << "  Static vs Runtime middleware (depth=10): "
                  << std::fixed << std::setprecision(1) << speedup << "x speedup\n";
    }

    // Cached vs uncached tool
    auto cached   = find("cached_tool (cache hit)");
    auto uncached = find("uncached_tool (same work)");
    if (cached && uncached && uncached->median_ns > 0) {
        double ratio = cached->median_ns / uncached->median_ns;
        std::cout << "  Cached vs uncached tool overhead: "
                  << std::fixed << std::setprecision(1) << ratio << "x\n";
    }

    std::cout << "\n";
    return 0;
}
