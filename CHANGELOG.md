# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Only
`0.3.0` is tagged; the entries before it are reconstructed from git history so
the record does not start at an arbitrary point.

## [Unreleased]

Everything merged since the `v0.3.0` tag.

- Fixed lifetime, merge, and parse defects in the agent core.
- Added pluggable observability middleware, with exporters for Arize Phoenix
  and Langfuse.
- Made vector stores runtime-pluggable (`AnyVectorStore`), with working Qdrant
  and Chroma adapters.
- Replaced three separate context triggers with one token budget.
- Repositioned the README around what the library actually is.
- Fixed explicit includes and non-ASCII literals that broke the non-Clang CI
  platforms.

## [0.3.0] - 2026-08-21

- Ported SSE streaming onto the refactored providers.
- Added the MIT license and CI workflow.
- Fixed a class-template CTAD issue AppleClang rejects that GCC and MSVC
  accept.
- Repositioned the README around the C++ agent-layer niche.
- Published a real SHA512 for the vcpkg port's source tarball.

## [0.2.0] - 2026-04-16 – 2026-05-12 (untagged)

- Added embeddings, a vector store, and a retriever.
- Migrated to the v2 concept-based architecture (`LLMModel<Provider, Kind>`),
  replacing the earlier interface-based design.
- Refactored providers to aggregate initialization, aligned the agent API with
  LangChain's naming, and added LLM-based summarization.

## [0.1.0] - 2026-04-14 (untagged)

- Initial implementation: chat, tool calling, nested agents.
- Follow-up correctness, safety, and structural fixes from the first code
  review.
