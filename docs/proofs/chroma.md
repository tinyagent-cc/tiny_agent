# Chroma: live verification

**Date:** 2026-08-21
**Backend:** `docker.io/chromadb/chroma:latest`, server reports API version `1.0.0`
**Image digest:** `sha256:1e0b73a187a28757c572acba508c46f48c9e8b0acaf5c20e6d95cdedce1acdf6`
**Runtime:** podman on macOS (podman-machine-default, libkrun)

Evidence that `ChromaVectorStore` (`include/tiny_agent/vectorstore/chroma.hpp`)
round-trips against a real Chroma server, beyond the offline
metadata-flattening and response-parsing tests in `tests/test_vectorstore_remote.cpp`.

## Start the container

```
podman run -d --name tacamp-chroma -p 8000:8000 docker.io/chromadb/chroma:latest
```

```
cbeb790bbed1e4e482634c4e4fadb3f28f8c67435dc535986a7706cf669c350d
```

Health check:

```
curl -s http://localhost:8000/api/v2/version
```

```
"1.0.0"
```

## Configure and build

Same tree as `qdrant.md`, one `cmake --preset default` and one
`cmake --build build -j 8`, both exit 0. See that file for the full command.

## ctest: offline (no env vars)

`test_vectorstore_remote` runs, and with no `CHROMA_URL` set its live
round-trip case returns via `REQUIRE_FALSE(url)` rather than executing.

```
      Start 17: test_vectorstore_remote
17/20 Test #17: test_vectorstore_remote ..........   Passed    0.44 sec
...
100% tests passed, 0 tests failed out of 20
Total Test time (real) =   6.47 sec
```

## ctest: live (CHROMA_URL set)

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

The Chroma case on its own, via doctest's test-case filter so the individual
assertions show instead of one ctest summary line:

```
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
./tests/test_vectorstore_remote --success -tc="live*"
```

```
===============================================================================
/tests/test_vectorstore_remote.cpp:227:
TEST CASE:  live Chroma round-trip

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
[doctest] test cases:  3 |  3 passed | 0 failed | 14 skipped
[doctest] assertions: 18 | 18 passed | 0 failed |
[doctest] Status: SUCCESS!
```

(This is the middle case of the same three-case run documented in
`qdrant.md`, which also covers the "live Qdrant behind a Retriever" case in
the same binary.) `hits[0].score` comes back `1.0` in these assertions
because the query vector is identical to `doc_a`'s stored vector, Chroma
reports distance `0.0`, and `ChromaVectorStore::parse_query_response` inverts
that to similarity `1.0`, which is the same inversion the offline test
`"Chroma query response unwraps one query and inverts distance"` checks
against fixed numbers.

## Example: `examples/19_vector_store`

```
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
./build/examples/19_vector_store
```

```

=== Chroma ===
indexed 5 documents

query: What CPU is in the Pi 5?
  0.555  The Raspberry Pi 5 uses a Broadcom BCM2712 quad-core Arm Cortex-A76.
  0.521  Cosine similarity measures the angle between two vectors, ignoring length.

query: how do agents find tools
  0.410  MCP lets an agent discover tools from a server over stdio or HTTP.
  0.387  Cosine similarity measures the angle between two vectors, ignoring length.

as a tool: [{"content":"Cosine similarity measures the angle between two vectors, ignoring length.","metadata":{"line":2,"source":"corpus"},"score":0.3806706666946411}]
```

Same ranking as the Qdrant and in-process runs of the same corpus in the same
process, with the score at the sixth decimal place diverging from Qdrant's
`0.3806706964969635`, float round-trip through two different HTTP APIs, not
a bug.

## State after the run

```
curl -s http://localhost:8000/api/v2/tenants/default_tenant/databases/default_database/collections
```

Returns the `tiny_agent_test` collection created by the ctest live run, empty
of points (the test calls `store.clear()` before returning) but still
registered as a collection, Chroma's `clear()` path in this library empties
points, it does not drop the collection. That is expected, not a defect: the
next `add_batch` call reuses it.

## Teardown

```
podman rm -f tacamp-chroma
```

Confirmed absent afterward, see `docs/integrations.md` for the final
`podman ps -a` across all three containers.
