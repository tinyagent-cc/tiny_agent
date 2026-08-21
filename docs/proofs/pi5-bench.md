# Raspberry Pi 5: benchmark proof

**Date:** 2026-08-21
**Device:** Raspberry Pi 5 Model B Rev 1.1, 4 cores, 16GB RAM, Debian 13 (trixie), kernel 6.12.75+rpt-rpi-2712, user `pi` (no sudo)
**Checkout:** `~/tiny_agent_cpp` on-device, standing deploy from a previous session
**Model server:** `llama-server` (llama.cpp, built from source in an earlier session) serving Qwen2.5-3B-Instruct Q4_K_M on port 8080, OpenAI-compatible, `--jinja -c 4096`. Already running when this session started; not stopped or restarted.

This proves the numbers in `docs/benchmarks.md`'s Raspberry Pi 5 v0.4 section against
current `origin/main` (`4c8eeea`, the v0.4 merge), not the older `feat/v0.3-m1` build
that produced the first Pi 5 entry in that file.

## Checkout had no `.git`

The on-device checkout at `~/tiny_agent_cpp` had been copied over rather than cloned;
`git status` returned `fatal: not a git repository`. It was already behind
`origin/main` by 39 files (the v0.4 observability, vector-store, tracing, and context
management work other sessions had merged since). Bootstrapped it into a real repo and
synced to the same tip as `origin/main`:

```
git init
git remote add origin https://github.com/rhajamor/tiny_agent.git
git fetch origin main
git add -A && git commit -m "snapshot pre-sync"
git diff --stat HEAD origin/main   # 39 files, 5274(+) 599(-)
git reset --hard origin/main
```

Result: `HEAD is now at 4c8eeea Merge pull request #3 from rhajamor/feat/v0.4`, matching
the tip of `origin/main` at fetch time. `build/` and `vcpkg_installed/` are gitignored
and untouched by the reset.

## Configure and build

Existing build directory, same toolchain the previous on-device session used
(`CMakeCache.txt`: `CMAKE_BUILD_TYPE=Release`, `CMAKE_TOOLCHAIN_FILE` pointed at
`~/tools/vcpkg`, triplet `arm64-linux`):

```
cd ~/tiny_agent_cpp/build
~/tools/cmake-4.4.2-linux-aarch64/bin/cmake ..
```

vcpkg pulled new dependencies the v0.4 source needs (fmt, nlohmann-json-schema-validator,
libenvpp among them) and finished in 219.6s:

```
All requested installations completed successfully in: 3.6 min
-- Configuring done (219.6s)
-- Generating done (0.1s)
-- Build files have been written to: /home/pi/tiny_agent_cpp/build
```

Full build:

```
~/tools/cmake-4.4.2-linux-aarch64/bin/cmake --build . -j4
```

Exit code 0, wall clock 647s (10.8 min) across the four cores. Tail of the log:

<details>
<summary>Build log, final 25 lines</summary>

```
[ 97%] Built target test_tracing
/home/pi/tiny_agent_cpp/bench/bench_agent.cpp: In lambda function:
/home/pi/tiny_agent_cpp/bench/bench_agent.cpp:218:19: warning: conversion to void will not access object of type 'const volatile tiny_agent::DynamicTool'
  218 |             (void)t;
      |                   ^
[ 98%] Linking CXX executable test_readme_snippets
[ 98%] Built target test_readme_snippets
[ 99%] Linking CXX executable bench_agent
[ 99%] Built target bench_agent
[100%] Linking CXX executable test_agent
[100%] Built target test_agent
```

</details>

The only warning in the whole build is the pre-existing `(void)t` nodiscard warning in
`bench/bench_agent.cpp:218`, unrelated to the v0.4 changes.

## Binary sizes

```
cd ~/tiny_agent_cpp/build
strip -o /tmp/bench_agent_stripped bench/bench_agent
strip -o /tmp/17_streaming_stripped examples/17_streaming
stat -c%s bench/bench_agent /tmp/bench_agent_stripped examples/17_streaming /tmp/17_streaming_stripped
```

| Binary | Unstripped | Stripped |
|---|---|---|
| `bench_agent` | 9,294,696 B (8.9 MB) | 7,958,312 B (7.6 MB) |
| `17_streaming` | 9,009,120 B (8.6 MB) | 7,761,656 B (7.4 MB) |

Both are static single binaries: TLS, JSON, the HTTP client and everything else linked
in, no runtime dependency beyond system libraries.

## ctest: offline

The repo carries a checked-in `.env` with real (but credit-exhausted) OpenAI, Claude and
Gemini keys. `test_agent.cpp` uses `libenvpp`'s `env::get_or`, which auto-loads that
`.env`; with it in place, the network-dependent sub-cases in `test_agent` try live calls
and fail on the exhausted OpenAI key rather than skipping cleanly:

```
cd ~/tiny_agent_cpp/build && ctest --output-on-failure
```

```
[ERROR] [llm] openai API error (status=429): {
    "error": { "message": "You have no credits remaining. ...", "code": "credit_balance_exhausted" }
}
...
95% tests passed, 1 tests failed out of 20
Total Test time (real) =   5.41 sec
The following tests FAILED:
	 20 - test_agent (Failed)
```

Moved `.env` aside for the genuinely offline run tiny_agent's tests are designed for
(each `test_agent` sub-case checks `keys().openai.empty()` etc. and calls `MESSAGE`
+ `return` when a key is absent):

```
cd ~/tiny_agent_cpp && mv .env /tmp/tiny_agent_env_backup
cd build && ctest --output-on-failure
mv /tmp/tiny_agent_env_backup ~/tiny_agent_cpp/.env
```

```
      Start 20: test_agent
20/20 Test #20: test_agent .......................   Passed    0.00 sec

100% tests passed out of 20
Total Test time (real) =   0.06 sec
```

`.env` is back in place at `~/tiny_agent_cpp/.env`; nothing in the repo's tracked files
touched it (it's gitignored). Flagging separately: those keys are live-looking secrets
sitting in plaintext in a home directory outside version control.

## bench_agent

```
cd ~/tiny_agent_cpp/build && ./bench/bench_agent
```

<details>
<summary>Full bench_agent output</summary>

```
╔═══════════════════════════════════════════════════════════════╗
║     tiny_agent_cpp Benchmark Suite                           ║
║     Target: Constrained Environments (RPi, Jetson, Arduino)  ║
╚═══════════════════════════════════════════════════════════════╝

=== MESSAGE & JSON OVERHEAD ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
Message::user (short)                                  75 ns       89 ns       74 ns      323 ns         11.1M
Message::user (256 chars)                              93 ns      101 ns       92 ns      128 ns          9.8M
Message::system (1KB)                                 130 ns      130 ns      129 ns        3 ns          7.7M
Message::tool_result (JSON 512B)                      112 ns      120 ns      111 ns       38 ns          8.3M
copy 50-msg history                                   4.6 us      4.9 us      4.5 us      966 ns        203.5K
copy 200-msg history (w/tools)                       20.8 us     21.0 us     20.2 us      1.6 us         47.7K
json::parse (sensor payload ~300B)                    7.9 us      8.7 us      7.7 us      1.2 us        114.5K
json::dump (sensor payload)                            2.2 us      2.2 us      2.1 us       70 ns        461.4K
ToolSchema create (full params)                       4.1 us      4.1 us      4.1 us      167 ns        241.7K
==============================================================================================================

=== TOOL REGISTRY ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
tool_lookup (5 tools)                                  74 ns       78 ns       74 ns       32 ns         12.7M
tool_lookup (20 tools)                                130 ns      138 ns      129 ns        9 ns          7.2M
tool_lookup (50 tools)                                111 ns      114 ns      111 ns        6 ns          8.8M
tool_lookup (100 tools)                               111 ns      106 ns       92 ns        9 ns          9.4M
tool_execute (arithmetic)                             426 ns      421 ns      407 ns       48 ns          2.4M
tool_create                                             2.4 us      2.4 us      2.4 us       84 ns        417.5K
schemas() (5 tools)                                     4.8 us      4.8 us      4.7 us      304 ns        208.7K
schemas() (20 tools)                                   28.3 us     28.3 us     25.0 us      404 ns         35.3K
schemas() (50 tools)                                   70.5 us     70.6 us     61.9 us      1.3 us         14.2K
tool_execute (JSON parse sensor)                        2.4 us      2.5 us      2.4 us      275 ns        408.1K
==============================================================================================================

=== MIDDLEWARE PIPELINE ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
chain_empty (baseline)                                 74 ns       74 ns       55 ns       35 ns         13.5M
chain_runtime (depth=1)                               129 ns      124 ns      111 ns        8 ns          8.0M
chain_runtime (depth=3)                               297 ns      303 ns      277 ns       24 ns          3.3M
chain_runtime (depth=5)                               667 ns      670 ns      648 ns       28 ns          1.5M
chain_runtime (depth=10)                                2.4 us      2.4 us      2.3 us       54 ns        409.7K
chain_runtime (depth=20)                                9.0 us      9.0 us      8.9 us      172 ns        110.7K
chain_static (depth=1)                                  74 ns       72 ns       55 ns        5 ns         13.8M
chain_static (depth=3)                                  74 ns       75 ns       74 ns        4 ns         13.3M
chain_static (depth=5)                                  74 ns       80 ns       74 ns        8 ns         12.4M
chain_static (depth=10)                                 93 ns       99 ns       92 ns        9 ns         10.0M
==============================================================================================================

=== BUILT-IN MIDDLEWARE ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
SystemPrompt (inject)                                 241 ns      243 ns      222 ns        6 ns          4.1M
SystemPrompt (already present)                          75 ns       82 ns       74 ns        9 ns         12.1M
TrimHistory<10> (20 msgs)                               2.6 us      2.6 us      2.6 us       51 ns        384.7K
TrimHistory<10> (50 msgs)                               7.1 us      7.1 us      7.0 us      323 ns        140.7K
TrimHistory<10> (100 msgs)                             14.0 us     14.0 us     13.9 us      281 ns         71.5K
TrimHistory<10> (200 msgs)                             28.0 us     28.0 us     27.8 us      283 ns         35.7K
Logging (off)                                           4.9 us      4.9 us      4.8 us       78 ns        204.5K
Logging (debug, sink=null)                              5.2 us      5.3 us      5.1 us      3.4 us        189.4K
PII (email, no match)                                   1.7 us      1.7 us      1.7 us      337 ns        574.7K
PII (email, 2 matches)                                  3.8 us      3.8 us      3.7 us       85 ns        265.2K
ModelCallLimit (under limit)                            93 ns       91 ns       74 ns        8 ns         11.0M
ToolCallLimit (under limit)                            204 ns      199 ns      185 ns       22 ns          5.0M
summarize<100,4> (20 msgs)                              6.4 us      6.4 us      6.2 us      1.2 us        155.3K
summarize<100,4> (50 msgs)                             17.4 us     17.5 us     17.1 us      3.5 us         57.1K
summarize<100,4> (100 msgs)                            34.5 us     34.5 us     34.0 us      589 ns         29.0K
Rationalize (with large result)                         1.8 us      1.8 us      1.7 us       51 ns        557.3K
Rationalize (no large results)                          759 ns      756 ns      703 ns       30 ns          1.3M
ContextEditing (30 msgs, trigger=50)                    2.8 us      2.8 us      2.6 us       72 ns        361.3K
==============================================================================================================

=== MEMORY STORE & CACHE ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
LRU<32> get (hit)                                       93 ns       95 ns       92 ns        7 ns         10.4M
LRU<32> get (miss)                                      74 ns       68 ns       55 ns        8 ns         14.6M
LRU<32> put (update)                                    93 ns       93 ns       74 ns       29 ns         10.7M
LRU<32> put (evict)                                     74 ns       78 ns       74 ns        7 ns         12.8M
LRU<128> get (hit)                                      93 ns       96 ns       92 ns        7 ns         10.4M
LRU<128> put (evict)                                    74 ns       80 ns       74 ns        8 ns         12.4M
LRU<512> get (hit)                                      93 ns       95 ns       92 ns        6 ns         10.5M
LRU<512> put (evict)                                    93 ns       89 ns       74 ns        6 ns         11.1M
ToolCache lookup (hit)                                  1.2 us      1.2 us      1.2 us       52 ns        812.7K
ToolCache lookup (miss)                                 1.9 us      1.9 us      1.9 us       46 ns        531.5K
ToolCache store                                         1.7 us      1.7 us      1.7 us       44 ns        580.0K
cached_tool (cache hit)                                 1.3 us      1.3 us      1.3 us       40 ns        753.7K
uncached_tool (same work)                               408 ns      414 ns      407 ns        9 ns          2.4M
==============================================================================================================

=== AGENT LOOP (MockLLM) ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
agent.run (no tools, no mw)                             1.6 us      1.6 us      1.6 us      592 ns        611.3K
agent.run (1 tool call)                                 6.5 us      6.5 us      6.4 us      812 ns        152.9K
agent.run (3 parallel tool calls)                      17.5 us     17.5 us     16.9 us      248 ns         57.2K
agent.run (3 mw + 1 tool)                               3.8 us      3.8 us      3.7 us       74 ns        263.7K
agent.run (FULL: 6 mw + 3 tools + 2 calls)              9.4 us      9.5 us      9.3 us      186 ns        105.7K
agent.chat (10 turns)                                  15.0 us     15.0 us     15.0 us      181 ns         66.5K
==============================================================================================================

=== CONSTRAINED ENVIRONMENT SCENARIOS ===
(Simulating Raspberry Pi / Jetson Nano workloads)

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
GPIO monitor (1 read cycle)                             7.1 us      7.1 us      7.1 us      459 ns        140.7K
Sensor aggregation (5 sensors)                         20.0 us     20.1 us     19.8 us      381 ns         49.8K
Managed chat (30 turns, trim=15)                      157.9 us    157.5 us    155.4 us      1.0 us          6.3K
PII-safe agent (3 PII filters)                         20.4 us     20.4 us     20.2 us      447 ns         48.9K
EMBEDDED PROD (6 mw + 3 tools)                          7.8 us      7.8 us      7.6 us      179 ns        128.5K
==============================================================================================================

╔═══════════════════════════════════════════════════════════════╗
║  SUMMARY — Key Numbers for Constrained Environments          ║
╚═══════════════════════════════════════════════════════════════╝

  Tool lookup (20 tools)                                130 ns  (7.2M ops/s)
  Tool execute (arithmetic)                             426 ns  (2.4M ops/s)
  Middleware chain (5 deep)                             667 ns  (1.5M ops/s)
  Static chain (5 deep)                                  74 ns  (12.4M ops/s)
  LRU<128> get (cache hit)                               93 ns  (10.4M ops/s)
  Cached tool (hit)                                     1.3 us  (753.7K ops/s)
  Agent run (no tools)                                  1.6 us  (611.3K ops/s)
  Agent run (full stack)                                9.4 us  (105.7K ops/s)
  GPIO monitor cycle                                    7.1 us  (140.7K ops/s)
  Embedded production agent                             7.8 us  (128.5K ops/s)
  json::parse (sensor payload)                           7.9 us  (114.5K ops/s)

  Static vs Runtime middleware (depth=5): 9.0x speedup
  Static vs Runtime middleware (depth=10): 26.3x speedup
  Cached vs uncached tool overhead: 3.2x
```

</details>

## 17_streaming against the local model

`local::ollama()` and `local::llamacpp()` both wrap the same OpenAI-compatible chat
provider; `llama-server`'s `/v1/models` reports itself under the model's file path, so:

```
cd ~/tiny_agent_cpp/build
OLLAMA_BASE_URL=http://localhost:8080 \
OLLAMA_MODEL="/home/pi/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf" \
./examples/17_streaming
```

No `/usr/bin/time` on-device and no sudo to install it, so peak RSS came from polling
`/proc/<pid>/status` (`VmHWM`) every 50ms for the process's lifetime:

```
> Tell me a short story about a curious robot.

In the city of Codeville, there lived a curious robot named Qwik. ...
[complete] 1162 chars

peak_rss_kb=2016
wall_time_sec=44.2
```

Peak client-side RSS: **2016 KB (~2.0 MB)**. `llama-server`'s own per-request timing log
for this run (task 765, the last one before the poll exited):

```
prompt eval time =     189.73 ms /     1 tokens (  189.73 ms per token,     5.27 tokens per second)
       eval time =   43969.08 ms /   242 tokens (  182.44 ms per token,     5.48 tokens per second)
      total time =   44158.82 ms /   243 tokens
   graphs reused =        998
```

Generation speed: **5.48 tok/s**. The prompt-eval line reflects a KV-cache hit
(`graphs reused = 998`, only 1 fresh token evaluated) from an earlier run against the
same fixed prompt in the example, not a cold-prompt measurement; the earlier `feat/v0.3-m1`
Pi 5 entry's 27.7 tok/s prompt-eval number was measured cold and isn't directly
comparable to this run's cached-prefix number for that reason.

## Cleanup

Removed this session's temp files (gcc's transient `.s` assembly files, build/RSS logs,
the `.env` backup) from `/tmp`, left everything else on the device untouched:

```
ssh pi@pi5.local 'rm -f /tmp/cc*.s /tmp/pi5_build.log /tmp/pi5_build_exit.txt \
  /tmp/build_start.txt /tmp/build_end.txt /tmp/streaming_out.log \
  /tmp/bench_agent_stripped /tmp/17_streaming_stripped /tmp/tiny_agent_env_backup'
ssh pi@pi5.local 'ls /tmp | head; pgrep -af "llama|ollama"'
```

```
chromium-hermes
chromium-hermes.log
conf.log
hermes-snap-0ec153daec9f.sh
liste_manuels_2026_college.pdf
liste_manuels_2026_college.txt
org.chromium.Chromium.ozaz4x
ssh-98S2ZnL2joMG
systemd-private-e649e86c052b47238b43bdcd46bee963-bluetooth.service-EJMrCk
systemd-private-e649e86c052b47238b43bdcd46bee963-polkit.service-p79XXm

37434 /home/pi/llama.cpp/build/bin/llama-server -m /home/pi/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf --port 8080 --jinja -c 4096
```

Everything left in `/tmp` predates this session (browser cache, a PDF, systemd sockets)
and none of it is mine. `llama-server` (PID 37434) is the same instance that was already
running before this session started; it was never touched. The `~/tiny_agent_cpp`
checkout and its `build/` directory stay in place as the standing on-device deploy.
