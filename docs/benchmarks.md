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

## macOS arm64 (M-series), 2026-08-21

| Metric | Value |
|---|---|
| `14_streaming` binary, Release, stripped | 7.7 MB |
| Unstripped | 9.8 MB |

Single binary, TLS included, no runtime dependencies beyond system libraries.
