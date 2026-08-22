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
| Weaviate | vector store | live-verified 2026-08-21 | [docs/proofs/weaviate.md](proofs/weaviate.md) |
| Redis | vector store | live-verified 2026-08-21 | [docs/proofs/redis.md](proofs/redis.md) |
| Milvus | vector store | live-verified 2026-08-21 | [docs/proofs/milvus.md](proofs/milvus.md) |
| Arize Phoenix | tracing exporter | live-verified 2026-08-21 | [docs/proofs/phoenix.md](proofs/phoenix.md) |
| Langfuse | tracing exporter | live-verified 2026-08-21 | [docs/proofs/langfuse.md](proofs/langfuse.md) |
| OTLP | tracing exporter | unit-tested | `tests/test_tracing.cpp` covers payload shaping; the "live OTLP export against a configured collector" case needs `TINY_AGENT_OTLP_ENDPOINT` against a real collector, not yet run |
| Console / in-memory exporters | tracing exporter | unit-tested | `include/tiny_agent/observability/console.hpp` (`stderr_exporter`, `noop_exporter`, `MemoryExporter`); no external backend to verify against, offline coverage is the whole story |
| rete_cpp | reflex + guardrail middleware, expert-system tool | header-only, optional | `tests/test_reflex.cpp`, `examples/21_reflex_agent.cpp` |

## What "live-verified" means here

Each live-verified row has a proof file with the date, the container image
and tag actually run, the exact commands, and pasted raw output from `ctest`
and from the doctest binary filtered to just the live cases, plus one piece
of server-side evidence beyond "the client didn't throw": collection or
keyspace state read back after the run for the five vector stores, Phoenix's
project list, and Langfuse's ingested observations read back through its
public API.

Qdrant, Chroma, Weaviate and Redis were also exercised through
`examples/19_vector_store` against the running containers; Milvus was
verified through its dedicated `test_vs_milvus` suite against a live server
instead. Phoenix was exercised through `examples/18_tracing` only as far as
exporter selection: the example needs a live LLM to produce a trace to
export, and none was configured in this environment, so the example itself
stops at "set OPENAI_API_KEY" after correctly picking the Phoenix exporter.
`test_tracing.cpp`'s live Phoenix and live Langfuse cases are what actually
post a span and are what this verification leans on for both.

## What's still open

OTLP needs a running collector (the OTel collector or a Phoenix/Jaeger OTLP
endpoint) pointed at by `TINY_AGENT_OTLP_ENDPOINT`; nothing library-side
blocks that, it just hasn't been run.
