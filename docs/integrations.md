# Integration status

Which of tiny_agent's external integrations have been checked against a real
backend, versus checked only offline against a captured or hand-built payload.
Offline coverage in `tests/` is solid across the board; this table is about
whether the wire format has actually been proven against the real thing, not
just the shape the code expects it to be.

| Integration | Kind | Status | Proof |
|---|---|---|---|
| Qdrant | vector store | live-verified 2026-08-21 | [docs/proofs/qdrant.md](proofs/qdrant.md) |
| Chroma | vector store | live-verified 2026-08-21 | [docs/proofs/chroma.md](proofs/chroma.md) |
| Arize Phoenix | tracing exporter | live-verified 2026-08-21 | [docs/proofs/phoenix.md](proofs/phoenix.md) |
| Langfuse | tracing exporter | live verification in progress | `tests/test_tracing.cpp` ("live Langfuse export against configured credentials"), currently exercised only offline |
| OTLP | tracing exporter | unit-tested | `tests/test_tracing.cpp` covers payload shaping; the "live OTLP export against a configured collector" case needs `TINY_AGENT_OTLP_ENDPOINT` against a real collector, not yet run |
| Console / in-memory exporters | tracing exporter | unit-tested | `include/tiny_agent/observability/console.hpp` (`stderr_exporter`, `noop_exporter`, `MemoryExporter`); no external backend to verify against, offline coverage is the whole story |

## What "live-verified" means here

Each of the three live-verified rows has a proof file with the date, the
container image and tag actually run, the exact commands, and pasted raw
output from `ctest` and from the doctest binary filtered to just the live
cases, plus one piece of server-side evidence beyond "the client didn't
throw" (Qdrant/Chroma collection state after the run, Phoenix's project list).

Qdrant and Chroma were also exercised through `examples/19_vector_store`
against the running containers. Phoenix was exercised through
`examples/18_tracing` only as far as exporter selection: the example needs a
live LLM to produce a trace to export, and none was configured in this
environment, so the example itself stops at "set OPENAI_API_KEY" after
correctly picking the Phoenix exporter. `test_tracing.cpp`'s live Phoenix
case is what actually posts a span and is the one this verification leans on.

## What's still open

Langfuse needs a real account (public/secret key pair) to exercise
`obs::langfuse_exporter` against; that hasn't happened yet, hence "in
progress" rather than "not verified". OTLP needs a running collector
(the OTel collector or a Phoenix/Jaeger OTLP endpoint) pointed at by
`TINY_AGENT_OTLP_ENDPOINT`; nothing library-side blocks that, it just hasn't
been run.
