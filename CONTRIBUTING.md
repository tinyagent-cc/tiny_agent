# Contributing

## Build

Needs a C++20 compiler, CMake 3.20+, and [vcpkg](https://github.com/microsoft/vcpkg).

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset default
cmake --build --preset default
```

`--preset release` builds optimized. Both presets set `CMAKE_TOOLCHAIN_FILE` to
vcpkg and pull `nlohmann-json`, `cpp-httplib`, `doctest`, `libenvpp`, and
`json-schema-validator` from `vcpkg.json` on first configure.

Build options (all default `ON` except the last):
`TINY_AGENT_BUILD_EXAMPLES`, `TINY_AGENT_BUILD_TESTS`, `TINY_AGENT_BUILD_BENCH`,
`TINY_AGENT_HNSWLIB` (needs `hnswlib`, default `OFF`).

## Test

```bash
ctest --preset default          # add -C Debug on multi-config generators
```

301 offline doctest cases across 19 files. No network, no API keys. That suite
is what CI runs and what a PR needs green.

`test_agent` calls real model providers and needs `OPENAI_API_KEY` (and reads a
repo-root `.env` if present). It is excluded from what "green" means for a PR
unless you touched an adapter — see below.

A few tests skip themselves unless a backend is reachable, checked through env
vars at test start:

```bash
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
  ctest --preset default -R test_vectorstore_remote

PHOENIX_BASE_URL=http://localhost:6006 ctest --preset default -R test_tracing
LANGFUSE_PUBLIC_KEY=pk-lf-… LANGFUSE_SECRET_KEY=sk-lf-… \
  ctest --preset default -R test_tracing
```

Bring the backends up with the official images, on the default ports the tests
expect:

```bash
docker run -d -p 6333:6333 qdrant/qdrant
docker run -d -p 8000:8000 chromadb/chroma
docker run -d -p 6006:6006 arizephoenix/phoenix
```

Langfuse needs a running instance (cloud or self-hosted); point
`LANGFUSE_BASE_URL` at it if it is not Cloud EU.

## Before you open a PR

- `ctest --preset default` is green. This is the bar; it is also what CI checks
  on Linux (x64 and arm64), macOS, and Windows.
- If you changed a provider, vector store, or observability adapter, run its
  live test against the real backend (API key or container, per above) and
  paste the output in the PR. Offline coverage proves the wire-format parsing;
  it does not prove the adapter still talks to the real service.
- If you changed anything under `docs/` or added a public type, update the doc
  that describes it. `README.md` and `docs/*.md` are read by users deciding
  whether to add this as a dependency; stale docs cost them more than no docs.
- `.clang-format` exists for new code and editor integration. Do not run it
  over files you did not otherwise touch — most of the tree is hand-formatted
  and a bulk reformat would swamp real diffs.

## What a PR looks like

Small and focused. One provider, one middleware, one bug. A PR that touches
five unrelated things is five PRs wearing a coat.

Conventional commit style for the title (`feat:`, `fix:`, `docs:`, `chore:`,
`refactor:`) — `git log` is the reference if the pattern is unclear.

## The rules that shape every change here

- **Header-only.** Everything a consumer needs is under `include/tiny_agent/`.
  No `.cpp` to compile into the library, no ABI to version.
- **No dependency a user did not ask for.** The core (`tiny_agent.hpp`) links
  only `nlohmann_json` and `cpp-httplib` — both already required by every
  provider. A new capability (a vector store backend, an exporter, hnswlib)
  gets its own header behind its own `#include`, so a project that never
  includes it never pays for it, in compile time or in linked code.
- **Concepts over virtual dispatch.** Providers, middleware, and vector stores
  are structural concepts (`is_chat`, `middleware_like`, `vector_store`), not
  base classes. `AnyVectorStore` and similar type-erased wrappers exist for the
  runtime-selection case; they are the exception, not the default shape of a
  new feature.

If a change does not fit one of these, open an issue first and say what it
needs to break and why.
