# Weaviate adapter, live verification

Run on **2026-08-21**, macOS 15 arm64, against
`docker.io/semitechnologies/weaviate:1.39.0`
(image `528f4de627eb`, the current stable release, tagged v1.39.0 on 2026-08-04).

Branch `feat/vs-weaviate`, worktree `.worktrees/vs-weaviate`, base `4c8eeea`.

## The server

```bash
podman run -d --name tacamp-weaviate -p 8080:8080 \
  -e AUTHENTICATION_ANONYMOUS_ACCESS_ENABLED=true \
  -e PERSISTENCE_DATA_PATH=/var/lib/weaviate \
  -e DEFAULT_VECTORIZER_MODULE=none \
  -e QUERY_DEFAULTS_LIMIT=25 \
  docker.io/semitechnologies/weaviate:1.39.0
```

```console
$ curl -s http://localhost:8080/v1/.well-known/ready -o /dev/null -w "%{http_code}\n"
200
$ curl -s http://localhost:8080/v1/meta | python3 -c "import json,sys; print(json.load(sys.stdin)['version'])"
1.39.0
```

The wire format came out of `GET /v1/swagger.json` on this container and out of
probing the endpoints by hand, not out of the model's memory of the API. The
same spec is published as
`openapi-specs/schema.json` at tag `v1.39.0`.

## What the API forced

Four probes changed the design. Each is reproduced here because each one is a
place a plausible-looking adapter would be wrong.

**Collection names are GraphQL type names, and the server rewrites yours.**

```console
$ curl -s -X POST http://localhost:8080/v1/schema -H 'Content-Type: application/json' \
    -d '{"class":"tiny_agent_test","vectorizer":"none"}' | head -c 40
{"class":"Tiny_agent_test",
$ curl -s -X POST http://localhost:8080/v1/schema -H 'Content-Type: application/json' \
    -d '{"class":"9bad-name","vectorizer":"none"}' -w "\nHTTP %{http_code}\n"
{"allowed":null,"error":[{"message":"'9bad-name' is not a valid class name"}]}
HTTP 422
```

Create `tiny_agent_test` and every later path spelled that way answers 404,
because what exists is `Tiny_agent_test`. The adapter capitalises at
construction and holds the capitalised name, so `collection()` reports what the
server holds.

**Near-vector search has no REST endpoint.** The only search path in the 1.39.0
spec is `POST /search/{collection}/near-text`, which needs a vectorizer module.
Bring-your-own-vectors search goes through GraphQL:

```console
$ curl -s -X POST http://localhost:8080/v1/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ Get { TinyAgentProbe(nearVector: {vector: [1,0,0]}, limit: 2) { content tiny_agent_id metadata _additional { id distance certainty } } } }"}'
{"data":{"Get":{"TinyAgentProbe":[{"_additional":{"certainty":1,"distance":0,"id":"11111111-1111-4111-a111-111111111111"},"content":"the cat sat","metadata":"{\"topic\":\"pets\"}","tiny_agent_id":"doc_a"},{"_additional":{"certainty":0.5,"distance":1,"id":"22222222-2222-4222-a222-222222222222"},"content":"the dog ran","metadata":"{\"topic\":\"pets\"}","tiny_agent_id":"doc_b"}]}}}
```

**GraphQL answers 200 whatever happened.** A dimension mismatch and a missing
collection both arrive as HTTP 200 with an `errors` array:

```console
$ curl -s -X POST http://localhost:8080/v1/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ Get { TinyAgentProbe(nearVector: {vector: [1,0]}, limit: 2) { content } } }"}' \
  -w "\nHTTP %{http_code}\n"
{"data":{"Get":{"TinyAgentProbe":null}},"errors":[{"locations":[{"column":9,"line":1}],"message":"explorer: get class: concurrentTargetVectorSearch): explorer: get class: vector search: object vector search at index tinyagentprobe: shard tinyagentprobe_fwGA8G2YdUrE: vector search: knn search: distance between entrypoint and query node: 3 vs 2: vector lengths don't match","path":["Get","TinyAgentProbe"]}]}
HTTP 200
```

**So does the batch endpoint, per object.** This write failed and the request
succeeded:

```console
$ curl -s -X POST http://localhost:8080/v1/batch/objects -H 'Content-Type: application/json' \
  -d '{"objects":[{"class":"TinyAgentProbe","id":"33333333-3333-4333-a333-333333333333","vector":[1.0,0.0],"properties":{"content":"bad dims"}}]}' \
  -w "\nHTTP %{http_code}\n"
[{"class":"TinyAgentProbe","creationTimeUnix":1787333582826,"id":"33333333-3333-4333-a333-333333333333","lastUpdateTimeUnix":1787333582826,"properties":{"content":"bad dims"},"vector":[1,0],"deprecations":null,"result":{"errors":{"error":[{"message":"Validate vector index for 33333333-3333-4333-a333-333333333333: new node has a vector with length 2. Existing nodes have vectors with length 3: vector dimensions do not match the index dimensions"}]},"status":"FAILED"}}]
HTTP 200
```

An adapter that checks only the status code drops writes without telling anyone.
`check_batch_response` reads the per-object result and throws.

## Offline suite, nothing set

```console
$ cd build && ctest --output-on-failure -E test_agent
      Start 20: test_vs_weaviate
20/20 Test #20: test_vs_weaviate .................   Passed    0.05 sec

100% tests passed, 0 tests failed out of 20

Total Test time (real) =   0.39 sec
```

`test_agent` is excluded because it wants real model API keys; it is the one
test in this repo that was already outside the offline set.

The Weaviate binary alone, with no `WEAVIATE_URL`:

```console
$ ./tests/test_vs_weaviate
[doctest] test cases: 22 | 22 passed | 0 failed | 0 skipped
[doctest] assertions: 72 | 72 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## Live suite

```console
$ WEAVIATE_URL=http://localhost:8080 ./tests/test_vs_weaviate
[doctest] test cases: 22 | 22 passed | 0 failed | 0 skipped
[doctest] assertions: 92 | 92 passed | 0 failed |
[doctest] Status: SUCCESS!
```

72 assertions offline against 92 live: the 20 that ran are the five
`WEAVIATE_URL`-gated cases (round-trip, nested metadata, ranking against
`FlatVectorStore`, behind a `Retriever`, through `AnyVectorStore`), not a guard
returning early.

Whole suite with the server up:

```console
$ WEAVIATE_URL=http://localhost:8080 ctest --output-on-failure -E test_agent
20/20 Test #20: test_vs_weaviate .................   Passed    2.01 sec

100% tests passed, 0 tests failed out of 20

Total Test time (real) =   2.32 sec
```

## Ranking consistency against FlatVectorStore

`examples/19_vector_store` indexes the same five-document corpus in both stores
with the same hash embeddings and asks the same two questions.

```console
$ WEAVIATE_URL=http://localhost:8080 ./build/examples/19_vector_store
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

=== Weaviate ===
indexed 5 documents

query: What CPU is in the Pi 5?
  0.555  The Raspberry Pi 5 uses a Broadcom BCM2712 quad-core Arm Cortex-A76.
  0.521  Cosine similarity measures the angle between two vectors, ignoring length.

query: how do agents find tools
  0.410  MCP lets an agent discover tools from a server over stdio or HTTP.
  0.387  Cosine similarity measures the angle between two vectors, ignoring length.

as a tool: [{"content":"Cosine similarity measures the angle between two vectors, ignoring length.","metadata":{"line":2,"source":"corpus"},"score":0.3806706666946411}]

=== AnyVectorStore (backend chosen at runtime) ===
indexed 5 documents
  0.564  Header-only C++ libraries need no build step from the consuming project.
```

Same order, same documents, and the scores agree to 0.38067069 against
0.38067067. Weaviate's cosine distance is `1 - cosine similarity`, so the
adapter's `1 - distance` lands back on the number `FlatVectorStore` computes;
the remaining gap is float round-tripping through JSON. The `line` and `source`
metadata comes back as the nested object that went in.

`live Weaviate ranks the same way FlatVectorStore does` makes the same check an
assertion, over a corpus whose vectors are deliberately not orthogonal so the
ordering is not free.

## Teardown

```console
$ podman rm -f tacamp-weaviate
tacamp-weaviate
$ podman ps -a --filter name=tacamp-weaviate
CONTAINER ID  IMAGE       COMMAND     CREATED     STATUS      PORTS       NAMES
```

Nothing left behind. The image stays in local storage; `podman rmi
semitechnologies/weaviate:1.39.0` removes that too.
