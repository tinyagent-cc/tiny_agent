# Redis vector store, live proof

**Date:** 2026-08-21
**Host:** macOS 15 (arm64), AppleClang 21.0.0, podman 5.7.1
**Image:** `docker.io/redis/redis-stack-server:latest`, built 2025-11-03, digest
`sha256:75cb09fe427163df4ba306b8d9668ac6af75555d88e3145171bb0324a57b00c1`
**Server:** Redis 7.4.7, RediSearch 2.10.20 (`ver 21020`)
**Branch:** `feat/vs-redis`

Everything below was run against that container. Nothing is reconstructed from
memory or from a previous session.

## Bring the server up

```console
$ podman run -d --name tacamp-redis -p 6379:6379 docker.io/redis/redis-stack-server:latest
07082e69674af652bf164e60ae84bfafd7c30dfecd83e2b011cb867a15642d25

$ podman exec tacamp-redis redis-cli PING
PONG

$ podman exec tacamp-redis redis-cli INFO server | grep redis_version
redis_version:7.4.7

$ podman exec tacamp-redis redis-cli MODULE LIST
name
search
ver
21020
path
/opt/redis-stack/lib/redisearch.so
```

## Build

```console
$ VCPKG_ROOT=~/.vcpkg cmake --preset default
-- Found httplib: …/vcpkg_installed/arm64-osx/include/httplib.h (found version "0.41.0")
-- Configuring done (3.8s)
-- Generating done (0.2s)

$ cmake --build build -j8
[100%] Built target test_vs_redis
```

## Offline: no `REDIS_URL`, whole suite

`test_agent` is excluded because it wants real provider API keys, which is true
on `main` too and has nothing to do with this change.

```console
$ ctest --exclude-regex test_agent
Test project /Users/riadh/git/tiny_agent_cpp/.worktrees/vs-redis/build
      Start  1: test_types
 1/20 Test  #1: test_types .......................   Passed    0.01 sec
…
20/20 Test #20: test_vs_redis ....................   Passed    0.01 sec

100% tests passed, 0 tests failed out of 20

Total Test time (real) =   5.75 sec
```

`test_vs_redis` on its own, same conditions:

```console
$ ./build/tests/test_vs_redis
[doctest] test cases:  29 |  29 passed | 0 failed | 0 skipped
[doctest] assertions: 147 | 147 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## Live: `REDIS_URL` set

```console
$ REDIS_URL=redis://localhost:6379 ctest --exclude-regex test_agent
20/20 Test #20: test_vs_redis ....................   Passed    0.02 sec

100% tests passed, 0 tests failed out of 20

Total Test time (real) =   0.25 sec

$ REDIS_URL=redis://localhost:6379 ctest -R test_vs_redis -V
20: [doctest] test cases:  29 |  29 passed | 0 failed | 0 skipped
20: [doctest] assertions: 163 | 163 passed | 0 failed |
20: [doctest] Status: SUCCESS!
```

163 assertions against 147 offline. The 16 extra are the three live cases, which
is how you can tell from the summary line alone that they ran rather than
short-circuited on the missing variable.

## Live round-trip, assertion by assertion

```console
$ REDIS_URL=redis://localhost:6379 ./build/tests/test_vs_redis -tc="live Redis round-trip" -s
TEST CASE:  live Redis round-trip

tests/test_vs_redis.cpp:397: SUCCESS: CHECK( store.size() == 3 ) is correct!
  values: CHECK( 3 == 3 )
tests/test_vs_redis.cpp:400: SUCCESS: REQUIRE( hits.size() == 2 ) is correct!
  values: REQUIRE( 2 == 2 )
tests/test_vs_redis.cpp:401: SUCCESS: CHECK( hits[0].id == "doc_a" ) is correct!
  values: CHECK( doc_a == doc_a )
tests/test_vs_redis.cpp:402: SUCCESS: CHECK( hits[0].content == "the cat sat on the mat" ) is correct!
  values: CHECK( the cat sat on the mat == the cat sat on the mat )
tests/test_vs_redis.cpp:403: SUCCESS: CHECK( hits[0].metadata["topic"] == "pets" ) is correct!
  values: CHECK( "pets" == pets )
tests/test_vs_redis.cpp:404: SUCCESS: CHECK( hits[0].score > hits[1].score ) is correct!
  values: CHECK( 1 >  0 )
tests/test_vs_redis.cpp:408: SUCCESS: CHECK( store.size() == 3 ) is correct!
  values: CHECK( 3 == 3 )
tests/test_vs_redis.cpp:411: SUCCESS: CHECK( store.size() == 0 ) is correct!
  values: CHECK( 0 == 0 )
```

## Ranking against FlatVectorStore

Four documents, one query, both stores. The ids have to come back in the same
order and the scores have to agree, because RediSearch cosine distance inverted
is the cosine similarity `FlatVectorStore` computes by hand.

```console
$ REDIS_URL=redis://localhost:6379 ./build/tests/test_vs_redis \
    -tc="live Redis ranks the same way FlatVectorStore does" -s
TEST CASE:  live Redis ranks the same way FlatVectorStore does

tests/test_vs_redis.cpp:436: SUCCESS: REQUIRE( redis_hits.size() == flat_hits.size() ) is correct!
  values: REQUIRE( 4 == 4 )
tests/test_vs_redis.cpp:439: SUCCESS: CHECK( redis_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_a == doc_a )
tests/test_vs_redis.cpp:442: SUCCESS: CHECK( redis_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.998049 == Approx( 0.998049 ) )
tests/test_vs_redis.cpp:439: SUCCESS: CHECK( redis_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_d == doc_d )
tests/test_vs_redis.cpp:442: SUCCESS: CHECK( redis_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.997441 == Approx( 0.997442 ) )
tests/test_vs_redis.cpp:439: SUCCESS: CHECK( redis_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_b == doc_b )
tests/test_vs_redis.cpp:442: SUCCESS: CHECK( redis_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.900102 == Approx( 0.900102 ) )
tests/test_vs_redis.cpp:439: SUCCESS: CHECK( redis_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_c == doc_c )
tests/test_vs_redis.cpp:442: SUCCESS: CHECK( redis_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.161409 == Approx( 0.161409 ) )
```

Same order, and the scores agree to five or six digits. `doc_d` differs in the
last digit (0.997441 against 0.997442) because the embedding crosses the wire as
float32 while `FlatVectorStore` keeps working in the host's float.

## What the server actually received

`redis-cli MONITOR` running while the round-trip test executed. This is the
adapter's real traffic, not the offline command-builder assertions.

```
1787333877.388933 "SCAN" "0" "MATCH" "tiny_agent_test:*" "COUNT" "500"
1787333877.389272 "FT.DROPINDEX" "tiny_agent_test"
1787333877.389788 "FT.INFO" "tiny_agent_test"
1787333877.390220 "FT.CREATE" "tiny_agent_test" "ON" "HASH" "PREFIX" "1" "tiny_agent_test:" "SCHEMA" "embedding" "VECTOR" "HNSW" "6" "TYPE" "FLOAT32" "DIM" "3" "DISTANCE_METRIC" "COSINE"
1787333877.392356 "HSET" "tiny_agent_test:doc_a" "content" "the cat sat on the mat" "metadata" "{\"topic\":\"pets\"}" "embedding" "\x00\x00\x80?\x00\x00\x00\x00\x00\x00\x00\x00"
1787333877.392433 "HSET" "tiny_agent_test:doc_b" "content" "the dog ran in the park" "metadata" "{\"topic\":\"pets\"}" "embedding" "\x00\x00\x00\x00\x00\x00\x80?\x00\x00\x00\x00"
1787333877.392444 "HSET" "tiny_agent_test:doc_c" "content" "quantum field theory" "metadata" "{\"topic\":\"physics\"}" "embedding" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80?"
1787333877.393705 "FT.SEARCH" "tiny_agent_test" "*" "LIMIT" "0" "0" "DIALECT" "2"
1787333877.394686 "FT.SEARCH" "tiny_agent_test" "*=>[KNN 2 @embedding $BLOB AS vector_score]" "PARAMS" "2" "BLOB" "\x00\x00\x80?\x00\x00\x00\x00\x00\x00\x00\x00" "RETURN" "3" "content" "metadata" "vector_score" "SORTBY" "vector_score" "LIMIT" "0" "2" "DIALECT" "2"
1787333877.396546 "HSET" "tiny_agent_test:doc_a" "content" "the cat sat on the mat" "metadata" "{\"topic\":\"pets\"}" "embedding" "\x00\x00\x80?\x00\x00\x00\x00\x00\x00\x00\x00"
1787333877.397110 "FT.SEARCH" "tiny_agent_test" "*" "LIMIT" "0" "0" "DIALECT" "2"
1787333877.397709 "SCAN" "0" "MATCH" "tiny_agent_test:*" "COUNT" "500"
1787333877.398154 "DEL" "tiny_agent_test:doc_b" "tiny_agent_test:doc_c" "tiny_agent_test:doc_a"
1787333877.398827 "FT.DROPINDEX" "tiny_agent_test"
1787333877.399158 "FT.SEARCH" "tiny_agent_test" "*" "LIMIT" "0" "0" "DIALECT" "2"
```

Three things this settles.

The three `HSET`s land inside 88 microseconds of each other, which is what a
pipelined write looks like: one socket write, then three replies read back.
Sequential round trips over loopback would show milliseconds between them.

`\x00\x00\x80?` is `1.0f` little-endian, and it travels as an argument, not as
escaped text. The RESP bulk-string length header is what makes that safe.

`FT.DROPINDEX` on line 2 answers with an error because the index does not exist
yet, and `clear()` treats "Unknown index name" as the state it was asking for
rather than a failure.

## Example 19, side by side with the in-process store

The same run, 64-dimensional hash embeddings over a five-document corpus, once
through `FlatVectorStore` and once through Redis.

```console
$ REDIS_URL=redis://localhost:6379 ./build/examples/19_vector_store

=== FlatVectorStore (in-process) ===
indexed 5 documents

query: What CPU is in the Pi 5?
  0.555  The Raspberry Pi 5 uses a Broadcom BCM2712 quad-core Arm Cortex-A76.
  0.521  Cosine similarity measures the angle between two vectors, ignoring length.

query: how do agents find tools
  0.410  MCP lets an agent discover tools from a server over stdio or HTTP.
  0.387  Cosine similarity measures the angle between two vectors, ignoring length.

as a tool: [{"content":"Cosine similarity measures the angle between two vectors, ignoring length.","metadata":{"line":2,"source":"corpus"},"score":0.3806706964969635}]

set QDRANT_URL to run the Qdrant backend
set CHROMA_URL to run the Chroma backend

=== Redis ===
indexed 5 documents

query: What CPU is in the Pi 5?
  0.555  The Raspberry Pi 5 uses a Broadcom BCM2712 quad-core Arm Cortex-A76.
  0.521  Cosine similarity measures the angle between two vectors, ignoring length.

query: how do agents find tools
  0.410  MCP lets an agent discover tools from a server over stdio or HTTP.
  0.387  Cosine similarity measures the angle between two vectors, ignoring length.

as a tool: [{"content":"Cosine similarity measures the angle between two vectors, ignoring length.","metadata":{"line":2,"source":"corpus"},"score":0.3806706666946411}]
```

`0.3806706964969635` against `0.3806706666946411`: seven matching digits, and
the divergence is the float32 round trip through the wire.

## FLAT and L2 accepted by the same server

The default is HNSW over cosine. The other combination the config offers, checked
against this server so the docs are not claiming an untested option:

```console
$ podman exec tacamp-redis redis-cli FT.CREATE flatidx ON HASH PREFIX 1 "flatidx:" \
    SCHEMA embedding VECTOR FLAT 6 TYPE FLOAT32 DIM 3 DISTANCE_METRIC L2
OK
$ podman exec tacamp-redis redis-cli FT.DROPINDEX flatidx
OK
```

## What plain Redis does instead

The docs say the store needs the search module. Checked rather than assumed,
against `redis:7-alpine` on a spare port:

```console
$ podman run -d --name tacamp-redis-plain -p 6399:6379 docker.io/library/redis:7-alpine
$ podman exec tacamp-redis-plain redis-cli FT.CREATE probe ON HASH PREFIX 1 "probe:" \
    SCHEMA embedding VECTOR FLAT 6 TYPE FLOAT32 DIM 3 DISTANCE_METRIC COSINE
ERR unknown command 'FT.CREATE', with args beginning with: 'probe' 'ON' 'HASH' 'PREFIX' '1' 'probe:' 'SCHEMA' 'embedding' 'VECTOR' 'FLAT' '6' 'TYPE' 'FLOAT32' 'DIM' '3' 'DISTANCE_METRIC'
$ podman rm -f tacamp-redis-plain
```

## The tests leave nothing behind

Both `FT._LIST` and `KEYS *` come back empty (one blank line each, since the
pipe is not a tty), and the keyspace is empty:

```console
$ podman exec tacamp-redis redis-cli FT._LIST | wc -l
       1
$ podman exec tacamp-redis redis-cli KEYS '*' | wc -l
       1
$ podman exec tacamp-redis redis-cli DBSIZE
0
```

## Teardown

```console
$ podman rm -f tacamp-redis
tacamp-redis

$ podman ps -a --filter name=tacamp-redis
CONTAINER ID  IMAGE       COMMAND     CREATED     STATUS      PORTS       NAMES
```

Header only, no rows. The container is gone and the port is free again.
