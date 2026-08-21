# Milvus adapter, live verification

Run on **2026-08-21**, macOS 26.6.2 arm64, against `docker.io/milvusdb/milvus:latest`,
which resolved that day to **Milvus 3.0.0** (image `703e4f1eb4fe`, digest
`sha256:49371c30af46b1013e4d3e0b980e691d81376d69cdbe1b372725baf1d7255862`).

Branch `feat/vs-milvus`, worktree `.worktrees/vs-milvus`, base `4aebd05`.

The wire format below came out of probing this container. Milvus has shipped
three different REST surfaces across its 2.x line, so nothing here was taken
from memory of the API.

## The server

The lightest standalone arrangement Milvus documents is one container with etcd
embedded and local file storage, so there is no companion etcd, no MinIO and no
Pulsar. Three things have to be right or it panics on startup:

- `DEPLOY_MODE=STANDALONE`, without which it decides it is in distributed mode
  and refuses the embedded etcd: `panic: embedded etcd can not be used under
  distributed mode`.
- A named volume rather than a bind mount for `/var/lib/milvus`. Under rootless
  podman the container's user cannot write into a bind-mounted host directory,
  and it dies with `failed to mkdir … permission denied`.
- `ETCD_CONFIG_PATH` pointing at an `embedEtcd.yaml`, mounted read-only.

```bash
cat > embedEtcd.yaml <<'EOF'
listen-client-urls: http://0.0.0.0:2379
advertise-client-urls: http://0.0.0.0:2379
quota-backend-bytes: 4294967296
auto-compaction-mode: revision
auto-compaction-retention: '1000'
EOF
: > user.yaml

podman volume create tacamp-milvus-data
podman run -d --name tacamp-milvus \
  --security-opt seccomp=unconfined \
  -e DEPLOY_MODE=STANDALONE \
  -e ETCD_USE_EMBED=true \
  -e ETCD_DATA_DIR=/var/lib/milvus/etcd \
  -e ETCD_CONFIG_PATH=/milvus/configs/embedEtcd.yaml \
  -e COMMON_STORAGETYPE=local \
  -v tacamp-milvus-data:/var/lib/milvus \
  -v $PWD/embedEtcd.yaml:/milvus/configs/embedEtcd.yaml:ro \
  -v $PWD/user.yaml:/milvus/configs/user.yaml:ro \
  -p 19530:19530 -p 9091:9091 \
  docker.io/milvusdb/milvus:latest \
  milvus run standalone
```

Health lives on 9091 and the API on 19530. Milvus multiplexes REST and gRPC onto
that one port, so there is no separate HTTP port to find.

```console
$ curl -s http://localhost:9091/healthz
OK
$ curl -s -X POST http://localhost:19530/v2/vectordb/collections/list \
    -H 'Content-Type: application/json' -d '{}'
{"code":0,"data":[]}
$ curl -s -o /dev/null -w '%{http_code}\n' -X POST \
    http://localhost:9091/v2/vectordb/collections/list -H 'Content-Type: application/json' -d '{}'
404
```

## What the API forced

Five probes changed the design. Each is reproduced because each is a place an
adapter written from the shape of the other three backends would be wrong.

### A refused request arrives as HTTP 200

```console
$ curl -s -w '\nhttp=%{http_code}\n' -X POST http://localhost:19530/v2/vectordb/entities/upsert \
  -H 'Content-Type: application/json' \
  -d '{"collectionName":"proof","data":[{"id":"bad","vector":[1.0,0.2],"content":"x","metadata":{}}]}'
{"code":1804,"message":"fail to deal the insert data, error: []float32 size 2 doesn't equal to vector dimension 3 of FloatVector: invalid parameter"}
http=200
```

A two-wide vector written into a three-wide collection is refused, and the
status line says the request succeeded. Every call in the adapter goes through
`check_code()`, which reads the body's `code` and throws with the server's own
message. Weaviate's batch endpoint has the same trap; Milvus has it on every
endpoint, including create and drop.

### Insert appends on a repeated primary key, upsert replaces

Starting from three rows:

```console
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/insert -d \
  '{"collectionName":"proof","data":[{"id":"doc_a", … }]}'
{"code":0,"cost":0,"data":{"insertCount":1,"insertIds":["doc_a"]}}
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/query -d \
  '{"collectionName":"proof","filter":"","outputFields":["count(*)"]}'
{"code":0,"cost":0,"data":[{"count(*)":4}]}

$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/upsert -d \
  '{"collectionName":"proof","data":[{"id":"doc_a", … }]}'
{"code":0,"cost":0,"data":{"upsertCount":1,"upsertIds":["doc_a"]}}
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/query -d \
  '{"collectionName":"proof","filter":"","outputFields":["count(*)"]}'
{"code":0,"cost":0,"data":[{"count(*)":3}]}
```

`insert` took the row count from 3 to 4 on a key that was already there. It does
not look at the primary key at all, so re-indexing a corpus doubles it and every
search after that reads a mixture of old and new. `upsert` put it back to 3.
`add_batch()` uses `/entities/upsert`, and `test_vs_milvus.cpp` covers this
live: two writes to `doc_a`, `size()` stays 1, and the content that comes back
is the second one.

### `get_stats` reports zero for rows that are really there

```console
$ curl -s -X POST http://localhost:19530/v2/vectordb/collections/get_stats -d \
  '{"collectionName":"proof"}'
{"code":0,"data":{"rowCount":0}}
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/query -d \
  '{"collectionName":"proof","filter":"","outputFields":["count(*)"]}'
{"code":0,"cost":0,"data":[{"count(*)":3}]}
```

Both calls ran against the same three-row collection seconds after the write.
`get_stats` counts sealed segments, and rows that are still in memory are in
none, so the endpoint whose name promises a row count is the one that lies.
`size()` runs the `count(*)` query.

### Reads need the collection loaded; writes do not

Creation starts an asynchronous load, so the state right after a create is
`LoadStateLoading`:

```console
$ curl -s -X POST http://localhost:19530/v2/vectordb/collections/create -d "$SCHEMA"
{"code":0,"data":{}}
$ curl -s -X POST http://localhost:19530/v2/vectordb/collections/get_load_state -d \
  '{"collectionName":"proof"}'
{"code":0,"data":{"loadProgress":0,"loadState":"LoadStateLoading"},"message":""}
$ sleep 3 && curl -s -X POST http://localhost:19530/v2/vectordb/collections/get_load_state -d \
  '{"collectionName":"proof"}'
{"code":0,"data":{"loadProgress":100,"loadState":"LoadStateLoaded"},"message":""}
```

Release it and the asymmetry shows: search and count both fail, and the write
still lands.

```console
$ curl -s -X POST http://localhost:19530/v2/vectordb/collections/release -d '{"collectionName":"proof"}'
{"code":0,"data":{}}
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/search -d \
  '{"collectionName":"proof","data":[[1.0,0.0,0.0]],"annsField":"vector","limit":1,"outputFields":["id"]}'
{"code":101,"message":"failed to search: collection not loaded[collection=468539437296064832]"}
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/query -d \
  '{"collectionName":"proof","filter":"","outputFields":["count(*)"]}'
{"code":101,"message":"failed to query: collection not loaded[collection=468539437296064832]"}
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/upsert -d \
  '{"collectionName":"proof","data":[{"id":"doc_d","vector":[0.0,0.0,1.0],"content":"written while released","metadata":{}}]}'
{"code":0,"cost":0,"data":{"upsertCount":1,"upsertIds":["doc_d"]}}
```

Qdrant, Chroma and Weaviate have no equivalent of this state, so it is the one
piece of the adapter with no counterpart in the other three. `ensure_loaded()`
polls `get_load_state`, asks for a load when the state is `LoadStateNotLoad`,
and waits out `LoadStateLoading`. Both reads go through it and the result is
cached after the first success.

One more thing `clear()` depends on: dropping a collection that is not there
succeeds rather than 404ing, which is the state `clear()` wanted.

```console
$ curl -s -X POST http://localhost:19530/v2/vectordb/collections/drop -d '{"collectionName":"never_existed"}'
{"code":0,"data":{}}
```

### The metric is a similarity, and the metadata is text

```console
$ curl -s -X POST http://localhost:19530/v2/vectordb/entities/search -d \
  '{"collectionName":"proof","data":[[0.9,0.4,0.1]],"annsField":"vector","limit":3,"outputFields":["id","content","metadata"]}'
{"code":0,"cost":0,"data":[{"content":"the cat sat on the mat","distance":0.97072536,"id":"doc_a","metadata":"{\"topic\":\"pets\",\"nested\":{\"deep\":1}}"},{"content":"the dog ran in the park","distance":0.6549371,"id":"doc_b","metadata":"{\"topic\":\"pets\"}"},{"content":"quantum field theory","distance":0.1407195,"id":"doc_c","metadata":"{\"topic\":\"physics\"}"}],"topks":[3]}
```

The field is called `distance` and under COSINE it holds the cosine similarity,
higher meaning closer. Chroma and Weaviate both report a real distance and their
adapters return `1 - distance`; doing that here would inverse the ranking. Only
`L2` needs converting, and the adapter negates it to keep the ordering.

`metadata` is a JSON field on the server and comes back over REST as its
serialised text, so `parse_metadata()` parses it back. Nesting survives
intact, unlike Chroma, which flattens non-scalar values.

Three more things the probe settled, none of them surprising but each one wrong
in the obvious alternative:

- The schema has to be spelled out. The quick setup (`{"collectionName":…,
  "dimension":3}`) gives an `Int64` primary key and the `Bounded` consistency
  level. The adapter declares a `VarChar` key, so caller ids go straight in, and
  `Strong`, so a search right after a write sees it.
- `elementTypeParams` values go as strings: `{"max_length":"512"}`,
  `{"dim":"3"}`.
- `indexParams` has to be in the create call. Without it the collection is
  created unindexed, never loads, and every search fails against a collection
  that describes as healthy.

## Offline tests

Nothing set, nothing running:

```console
$ ctest --test-dir build --output-on-failure -E test_agent
      Start 20: test_vs_milvus
20/20 Test #20: test_vs_milvus ...................   Passed    0.02 sec

100% tests passed, 0 tests failed out of 20

Total Test time (real) =   6.16 sec

$ ./build/tests/test_vs_milvus
[doctest] test cases: 31 | 31 passed | 0 failed | 0 skipped
[doctest] assertions: 97 | 97 passed | 0 failed |
[doctest] Status: SUCCESS!
```

The seven live cases pass vacuously without `MILVUS_URL`, which is where the
assertion count falls from 121 to 97.

## Live tests

```console
$ MILVUS_URL=http://localhost:19530 ctest --test-dir build --output-on-failure -R test_vs_milvus
    Start 20: test_vs_milvus
1/1 Test #20: test_vs_milvus ...................   Passed   17.45 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =  17.45 sec

$ MILVUS_URL=http://localhost:19530 ./build/tests/test_vs_milvus
[doctest] test cases:  31 |  31 passed | 0 failed | 0 skipped
[doctest] assertions: 121 | 121 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## Ranking against FlatVectorStore

The same three documents and the same query go into `FlatVectorStore` and into
Milvus, and the two are compared hit by hit:

```console
$ MILVUS_URL=http://localhost:19530 ./build/tests/test_vs_milvus \
    -tc="live Milvus ranks the same way FlatVectorStore does" -s
TEST CASE:  live Milvus ranks the same way FlatVectorStore does

SUCCESS: REQUIRE( milvus_hits.size() == 3 ) is correct!
  values: REQUIRE( 3 == 3 )
SUCCESS: CHECK( milvus_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_a == doc_a )
SUCCESS: CHECK( milvus_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.970725 == Approx( 0.970725 ) )
SUCCESS: CHECK( milvus_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_b == doc_b )
SUCCESS: CHECK( milvus_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.654937 == Approx( 0.654937 ) )
SUCCESS: CHECK( milvus_hits[i].id == flat_hits[i].id ) is correct!
  values: CHECK( doc_c == doc_c )
SUCCESS: CHECK( milvus_hits[i].score == doctest::Approx(flat_hits[i].score).epsilon(0.001) ) is correct!
  values: CHECK( 0.14072 == Approx( 0.14072 ) )

[doctest] test cases: 1 | 1 passed | 0 failed | 30 skipped
[doctest] assertions: 7 | 7 passed | 0 failed |
```

Same order, and the scores agree to six decimals with the `0.97072536`,
`0.6549371` and `0.1407195` the raw REST probe returned above. The tolerance in
the test is 0.001; the agreement is far tighter than that, because Milvus under
COSINE computes the same number `FlatVectorStore` does and the adapter passes it
through untouched.

## Teardown

```console
$ podman rm -f tacamp-milvus && podman volume rm tacamp-milvus-data
$ podman ps -a --filter name=tacamp
CONTAINER ID  IMAGE       COMMAND     CREATED     STATUS      PORTS       NAMES
```
