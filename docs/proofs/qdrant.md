# Qdrant: live verification

**Date:** 2026-08-21
**Backend:** `docker.io/qdrant/qdrant:latest`, server reports `1.17.1` (commit `eabee371fda447974a94d29fbaa675a6a596cc7b`)
**Image digest:** `sha256:94728574965d17c6485dd361aa3c0818b325b9016dac5ea6afec7b4b2700865f`
**Runtime:** podman on macOS (podman-machine-default, libkrun)

This is evidence that `QdrantVectorStore` (`include/tiny_agent/vectorstore/qdrant.hpp`)
round-trips against a real Qdrant server. The offline payload-shaping tests in
`tests/test_vectorstore_remote.cpp` cover the request/response shape; this
proves the shape actually works against Qdrant 1.17.1.

## Start the container

```
podman run -d --name tacamp-qdrant -p 6333:6333 docker.io/qdrant/qdrant:latest
```

```
3a94a654ef50c64634794a257c172cedec2d4f3e4b25b4256293564b9843d025
```

Health check:

```
curl -s http://localhost:6333/
```

```json
{"title":"qdrant - vector search engine","version":"1.17.1","commit":"eabee371fda447974a94d29fbaa675a6a596cc7b"}
```

## Configure and build

```
VCPKG_ROOT=/Users/riadh/.vcpkg cmake --preset default
VCPKG_ROOT=/Users/riadh/.vcpkg cmake --build build -j 8
```

Both completed with exit code 0. Full logs are the `ctest` runs below, which only
execute against a built tree.

## ctest: offline (no env vars)

```
cd build && ctest --output-on-failure --timeout 120
```

`test_vectorstore_remote` still runs here; with no `QDRANT_URL` set, its three
live-round-trip cases hit `REQUIRE_FALSE(url)` and return, so they count as
passed-but-empty rather than failed.

```
      Start 17: test_vectorstore_remote
17/20 Test #17: test_vectorstore_remote ..........   Passed    0.44 sec
...
100% tests passed, 0 tests failed out of 20
Total Test time (real) =   6.47 sec
```

## ctest: live (QDRANT_URL set)

```
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
PHOENIX_BASE_URL=http://localhost:6006 \
ctest --output-on-failure --timeout 120
```

```
      Start 17: test_vectorstore_remote
17/20 Test #17: test_vectorstore_remote ..........   Passed    1.21 sec
...
100% tests passed, 0 tests failed out of 20
Total Test time (real) =   1.50 sec
```

The Qdrant-specific portion, isolated with doctest's own test filter so the
assertions are visible instead of hidden behind ctest's one-line-per-binary
summary:

```
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
./tests/test_vectorstore_remote --success -tc="live*"
```

```
===============================================================================
/tests/test_vectorstore_remote.cpp:216:
TEST CASE:  live Qdrant round-trip

/tests/test_vectorstore_remote.cpp:199: SUCCESS: CHECK( store.size() == 3 ) is correct!
  values: CHECK( 3 == 3 )

/tests/test_vectorstore_remote.cpp:202: SUCCESS: REQUIRE( hits.size() == 2 ) is correct!
  values: REQUIRE( 2 == 2 )

/tests/test_vectorstore_remote.cpp:203: SUCCESS: CHECK( hits[0].id == "doc_a" ) is correct!
  values: CHECK( doc_a == doc_a )

/tests/test_vectorstore_remote.cpp:204: SUCCESS: CHECK( hits[0].content == "the cat sat on the mat" ) is correct!
  values: CHECK( the cat sat on the mat == the cat sat on the mat )

/tests/test_vectorstore_remote.cpp:205: SUCCESS: CHECK( hits[0].metadata["topic"] == "pets" ) is correct!
  values: CHECK( "pets" == pets )

/tests/test_vectorstore_remote.cpp:206: SUCCESS: CHECK( hits[0].score > hits[1].score ) is correct!
  values: CHECK( 1 >  0 )

/tests/test_vectorstore_remote.cpp:210: SUCCESS: CHECK( store.size() == 3 ) is correct!
  values: CHECK( 3 == 3 )

/tests/test_vectorstore_remote.cpp:213: SUCCESS: CHECK( store.size() == 0 ) is correct!
  values: CHECK( 0 == 0 )

===============================================================================
/tests/test_vectorstore_remote.cpp:238:
TEST CASE:  live Qdrant behind a Retriever

/tests/test_vectorstore_remote.cpp:252: SUCCESS: REQUIRE( hits.size() == 1 ) is correct!
  values: REQUIRE( 1 == 1 )

/tests/test_vectorstore_remote.cpp:253: SUCCESS: CHECK( hits[0].content == "the cat sat" ) is correct!
  values: CHECK( the cat sat == the cat sat )

===============================================================================
[doctest] test cases:  3 |  3 passed | 0 failed | 14 skipped
[doctest] assertions: 18 | 18 passed | 0 failed |
[doctest] Status: SUCCESS!
```

("live Chroma round-trip" is the third case in that run; see `chroma.md` for
its output.) The 14 skipped cases are the offline payload-shaping tests in the
same binary and the Chroma/OTLP/Langfuse live cases that this run did not
configure, doctest counts a case filtered out by `-tc` as skipped, not run.

## Example: `examples/19_vector_store`

```
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
./build/examples/19_vector_store
```

```

=== Qdrant ===
indexed 5 documents

query: What CPU is in the Pi 5?
  0.555  The Raspberry Pi 5 uses a Broadcom BCM2712 quad-core Arm Cortex-A76.
  0.521  Cosine similarity measures the angle between two vectors, ignoring length.

query: how do agents find tools
  0.410  MCP lets an agent discover tools from a server over stdio or HTTP.
  0.387  Cosine similarity measures the angle between two vectors, ignoring length.

as a tool: [{"content":"Cosine similarity measures the angle between two vectors, ignoring length.","metadata":{"line":2,"source":"corpus"},"score":0.3806706964969635}]
```

Scores match the in-process `FlatVectorStore` run in the same output byte for
byte on the ranking and to three decimal places on score, which is what you
want from a store swap that is supposed to be a config change, not a behavior
change.

## State after the run

```
curl -s http://localhost:6333/collections
```

```json
{"result":{"collections":[]},"status":"ok","time":0.000040333}
```

Empty: the live test round-trip and the example both call `store.clear()` on
their way out, and the `Retriever` test drops its collection through the same
path.

## Teardown

```
podman rm -f tacamp-qdrant
```

Confirmed absent afterward, see `docs/integrations.md` for the final
`podman ps -a` across all three containers.
