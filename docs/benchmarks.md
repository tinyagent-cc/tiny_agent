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

## macOS arm64 (M-series), 2026-08-21

| Metric | Value |
|---|---|
| `14_streaming` binary, Release, stripped | 7.7 MB |
| Unstripped | 9.8 MB |

Single binary, TLS included, no runtime dependencies beyond system libraries.
