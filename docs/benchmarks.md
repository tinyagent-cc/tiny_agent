# Measured footprint and performance

All numbers measured, not estimated. Dates and setups below.

## Raspberry Pi 5 (16GB, aarch64, CPU only), 2026-08-21

Stack: tiny_agent `feat/v0.3-m1` release build, llama.cpp `llama-server` (built from source the same day), Qwen2.5-3B-Instruct Q4_K_M.

| Metric | Value |
|---|---|
| `14_streaming` client RSS during an active streamed response | 5.7 MB |
| Model generation speed (server side) | 5.8 tok/s |
| Prompt eval speed (server side) | 27.7 tok/s |
| Full offline test suite on the Pi | 15/15 pass, 0.06 s |

The agent layer is not the cost. The model is. tiny_agent's orchestration adds under 6 MB of RSS on top of whatever your inference server needs.

## Raspberry Pi 5 (16GB, aarch64, CPU only), 2026-08-21 (v0.4)

Same device as above, rebuilt against current `main` (`4c8eeea`, the v0.4 merge:
observability, vector stores, tracing, context management) instead of `feat/v0.3-m1`.
Stack: llama.cpp `llama-server` (same instance from the v0.3 run, still up), Qwen2.5-3B-Instruct Q4_K_M.

Hardware: Raspberry Pi 5 Model B Rev 1.1, 4 cores, 16GB RAM, Debian 13 (trixie).
Toolchain: cmake 4.4.2, g++ 14.2.0 (Debian 14.2.0-19), vcpkg 2026-07-27, user-local (no sudo).

| Metric | Value |
|---|---|
| Full Release build (4 threads, cold vcpkg deps for new v0.4 code) | 647 s (10.8 min) |
| `bench_agent` binary, Release, stripped | 7.6 MB |
| `17_streaming` binary, Release, stripped | 7.4 MB |
| `17_streaming` client peak RSS during a streamed response | 2.0 MB |
| Model generation speed (server side) | 5.48 tok/s |
| Offline `ctest` suite on the Pi | 20/20 pass, 0.06 s |

RSS dropped from 5.7 MB on the v0.3 build to 2.0 MB here, even with the v0.4 code (observability, vector stores, tracing) linked in; generation speed is unchanged within noise (5.5-5.8 tok/s across both runs, bound by the model, not the agent layer). Prompt-eval speed isn't reported for this run: the fixed prompt in `17_streaming` hit the server's KV cache from an earlier run, so the measurement would understate a cold prompt rather than compare fairly against the 27.7 tok/s above.

Full commands, raw build/test/bench output, and the on-device git bootstrap (the checkout had no `.git`) are in [`docs/proofs/pi5-bench.md`](proofs/pi5-bench.md).

<details>
<summary>Raw bench_agent summary block</summary>

```
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

## NVIDIA Jetson Orin Nano Super (8GB, aarch64, CUDA), 2026-08-21

First port to the Jetson, on the same `4c8eeea` v0.4 merge as the Pi 5 run above, with
the same model and quantization, so the two sections compare directly.

Hardware: Jetson Orin Nano Engineering Reference Developer Kit Super, 6 cores
(Cortex-A78AE at 1.73 GHz), 7.4GB LPDDR5 shared between CPU and GPU, Ampere GA10B GPU
at compute capability 8.7, `nvpmodel` MAXN_SUPER.
Toolchain: JetPack 6.2 (L4T R36.5.0, kernel 5.15.185-tegra, Ubuntu 22.04.5), g++ 11.4.0,
cmake 3.31.6 and vcpkg 2026-07-27 user-local under `~/tools`, CUDA 12.6.68 from the
NVIDIA Jetson apt repo (root needed for that one, nothing else).
Stack: llama.cpp `873e5d8` built from source with `GGML_CUDA=ON`, Qwen2.5-3B-Instruct Q4_K_M.

| Metric | Value |
|---|---|
| First-time `cmake --preset release` (all five vcpkg deps from source, OpenSSL included) | 245 s |
| Clean full Release build (6 threads, deps already built) | 524 s (8.7 min) |
| `bench_agent` binary, Release, stripped | 7.7 MB |
| `17_streaming` binary, Release, stripped | 7.6 MB |
| `17_streaming` client peak RSS during a streamed response | 5.1 MB |
| Model generation speed, GPU, all 36 layers offloaded | 23.85 tok/s |
| Prompt eval speed, GPU | 874.77 tok/s |
| Model generation speed, CPU only, 6 threads | 12.75 tok/s |
| Prompt eval speed, CPU only | 40.39 tok/s |
| Offline `ctest` suite on the Jetson | 20/20 pass, 0.16 s |

The GPU is worth 1.9x on generation and 22x on prompt eval over the same board's CPU, and
4.4x on generation over the Pi 5 above. Prompt eval is where the gap really opens, which
matters for agent work: every tool result and every middleware-injected message is prompt
tokens the model has to re-read.

`-ngl 0` is not a CPU measurement here. With a CUDA backend loaded, llama.cpp still offloads
the large prompt matmuls and reports 598 tok/s on `pp512`. The CPU rows above use `-dev none`.

The client footprint does not depend on the backend. Peak RSS was 5.0-5.2 MB across five
runs, GPU-backed at 23 tok/s and CPU-backed at 9 tok/s alike, and a 1153-char response cost
the same as an 1820-char one. That is 5.1 MB against 2.0 MB for the Pi 5 on identical source,
measured the same way (`VmHWM` polled over the process lifetime). The difference is the
toolchain, not the workload: Ubuntu 22.04 with glibc 2.35 and g++ 11.4 here, Debian 13 with
g++ 14.2 on the Pi. Same story in the agent benchmarks below, where the Pi's older, slower
silicon still posts better numbers than this board on several rows. If you are size- or
latency-sensitive on a Jetson, a newer toolchain than the one JetPack 6.2 ships is worth
having.

One example does not compile on g++ 11.4. `16_deep_agent_custom.cpp` calls
`init_chat_model("openai:gpt-4o-mini", {.api_key = key})`, and g++ 11 keeps both the
two-argument and three-argument overloads in the candidate set when the second argument is a
braced initializer, then cannot choose. Clang and newer GCC discard the wrong candidate. The
other 52 targets, the whole test suite and the bench build clean; the failure is recorded
rather than patched, since the fix belongs in a code change.

Full commands, raw build/test/bench output, the CUDA bring-up and the RSS methodology are in
[`docs/proofs/jetson-bench.md`](proofs/jetson-bench.md).

<details>
<summary>Raw bench_agent summary block</summary>

```
╔═══════════════════════════════════════════════════════════════╗
║  SUMMARY — Key Numbers for Constrained Environments          ║
╚═══════════════════════════════════════════════════════════════╝

  Tool lookup (20 tools)                                128 ns  (7.3M ops/s)
  Tool execute (arithmetic)                             544 ns  (1.8M ops/s)
  Middleware chain (5 deep)                             704 ns  (1.4M ops/s)
  Static chain (5 deep)                                  96 ns  (10.7M ops/s)
  LRU<128> get (cache hit)                               96 ns  (8.9M ops/s)
  Cached tool (hit)                                     1.6 us  (606.4K ops/s)
  Agent run (no tools)                                  1.8 us  (551.7K ops/s)
  Agent run (full stack)                                10.5 us  (95.1K ops/s)
  GPIO monitor cycle                                    8.0 us  (125.0K ops/s)
  Embedded production agent                              9.9 us  (101.0K ops/s)
  json::parse (sensor payload)                           8.0 us  (124.1K ops/s)

  Static vs Runtime middleware (depth=5): 7.3x speedup
  Static vs Runtime middleware (depth=10): 25.0x speedup
  Cached vs uncached tool overhead: 3.4x
```

</details>

## macOS arm64 (M-series), 2026-08-21

| Metric | Value |
|---|---|
| `14_streaming` binary, Release, stripped | 7.7 MB |
| Unstripped | 9.8 MB |

Single binary, TLS included, no runtime dependencies beyond system libraries.
