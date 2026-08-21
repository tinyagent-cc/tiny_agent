# Vector stores and retrieval

`#include <tiny_agent/retriever.hpp>` gets you the interface, the in-process
store and the `Retriever`. Server-backed stores are separate headers because
each one needs a service running.

## The interface

Four methods. That is the whole contract:

```cpp
template<typename T>
concept vector_store = requires(T& s, const std::string& id,
                                const std::string& content,
                                const std::vector<float>& embedding,
                                const json& metadata, int top_k) {
    s.add(id, content, embedding, metadata);
    { s.search(embedding, top_k) } -> std::same_as<std::vector<SearchResult>>;
    { s.size() }                   -> std::convertible_to<std::size_t>;
    s.clear();
};
```

`search()` returns a **similarity**: higher means closer, best first. Backends
that natively report a distance convert before returning, so the same retrieval
code reads the same way against every store. Chroma reports cosine *distance*,
for instance, and its adapter returns `1 - distance`.

A store that can take a batch in one request adds `add_batch(vector<Document>)`
and satisfies `batch_vector_store`. Call `add_documents(store, docs)` and you
get the batch path where it exists and a loop where it does not.

## Choosing a store

| Store | Header | Needs |
|---|---|---|
| `FlatVectorStore` | `retriever.hpp` | nothing, brute-force cosine over a vector |
| `HnswVectorStore` | `vectorstore/hnswlib.hpp` | hnswlib (`-DTINY_AGENT_HNSWLIB=ON`) |
| `QdrantVectorStore` | `vectorstore/qdrant.hpp` | a running Qdrant |
| `ChromaVectorStore` | `vectorstore/chroma.hpp` | a running Chroma |
| `WeaviateVectorStore` | `vectorstore/weaviate.hpp` | a running Weaviate |
| `RedisVectorStore` | `vectorstore/redis.hpp` | a Redis with the search module |
| `MilvusVectorStore` | `vectorstore/milvus.hpp` | a running Milvus |
| `AnyVectorStore` | `retriever.hpp` | nothing, wraps any of the above |

`FlatVectorStore` is an O(n) scan. It is the right choice up to a few thousand
documents and it keeps the dependency count at zero.

## Retriever

```cpp
#include <tiny_agent/retriever.hpp>
#include <tiny_agent/providers/openai.hpp>

Retriever retriever{OpenAIEmbedding{"text-embedding-3-small", key}, 4};

retriever.add_documents({"the cat sat on the mat", "quantum field theory"},
                        {json{{"topic", "pets"}}, json{{"topic", "physics"}}});

for (const auto& hit : retriever.query("where is the cat?", 2))
    std::cout << hit.score << "  " << hit.content << "\n";
```

Hand it to an agent as a tool:

```cpp
AgentConfig cfg;
cfg.tools.push_back(retriever.as_tool("search_docs", "Search the local corpus"));
```

`as_tool()` borrows the retriever, so the retriever must outlive the tool and
must not be moved afterwards. When the tool should own its retriever instead,
use the free `retriever_as_tool(std::shared_ptr<Retriever>, …)`.

## Qdrant

```cpp
#include <tiny_agent/vectorstore/qdrant.hpp>

QdrantVectorStore store{"http://localhost:6333", "my_docs",
                        QdrantConfig{.api_key = "…", .distance = "Cosine"}};
```

The collection is created on the first write, sized from the first embedding.
Searches go to `POST /points/query`, the endpoint Qdrant now recommends over
`/points/search`; the response parser reads either shape.

**Point ids.** Qdrant accepts only an unsigned integer or a UUID, and rejects
anything else with `400 … is not a valid point ID`. Every other store here takes
a free-form string, so the adapter hashes your id into a UUID for the wire and
keeps the original in the payload. `search()` gives back the id you stored. The
mapping is deterministic, so re-adding the same id updates the point rather than
duplicating it.

`size()` asks the server (`POST /points/count`) rather than counting locally, so
a collection another process also writes to reports the truth. `clear()` deletes
the collection; the next write recreates it.

## Chroma

```cpp
#include <tiny_agent/vectorstore/chroma.hpp>

ChromaVectorStore store{"http://localhost:8000", "my_docs",
                        ChromaConfig{.space = "cosine"}};
```

Chroma's v2 API keys every record path by the collection's **UUID**, so the
first call resolves the name via `get_or_create` and caches the id. The one
exception is deleting a collection, which takes the **name**. Deleting by UUID
answers 404, so a `clear()` written the obvious way looks like it worked and
changes nothing. The adapter uses the name.

Metadata values must be scalars. Nested objects and arrays are stored as their
JSON text rather than being dropped, so nothing is lost, just flattened.

`clear()` deletes the collection rather than deleting records, because Chroma's
record-delete endpoint removes everything when given neither `ids` nor a `where`
clause.

## Weaviate

```cpp
#include <tiny_agent/vectorstore/weaviate.hpp>

WeaviateVectorStore store{"http://localhost:8080", "my_docs",
                          WeaviateConfig{.api_key = "…", .distance = "cosine"}};
```

HTTP only, no gRPC and no client library. The collection is created on the first
write with `vectorizer: none`, which is what makes the server keep the vectors
you hand it instead of embedding the text with whatever
`DEFAULT_VECTORIZER_MODULE` it was started with. No dimension goes in the schema:
Weaviate sizes the index from the first object, where Qdrant wants the width at
creation time.

**Collection names.** A Weaviate collection is a GraphQL type, so its name has to
be a GraphQL type name. Pass `my_docs` and the server stores `My_docs`, then
answers 404 for every path spelled the way you asked. The adapter capitalises at
construction and works with the capitalised form throughout, so `collection()`
reports the name the server actually holds. A name GraphQL cannot express, like
`my-docs` or `9docs`, throws rather than failing later with a 422.

**Object ids** must be UUIDs, the same constraint Qdrant has, so the adapter uses
the same fix: hash your id into a UUID for the wire, keep the original in a
`tiny_agent_id` property, and give it back from `search()`. The mapping is
deterministic, so re-adding an id updates the object.

**Search is GraphQL.** Weaviate 1.39 has one REST search path,
`POST /v1/search/{collection}/near-text`, and it needs a vectorizer module.
Bring-your-own-vector search runs through `POST /v1/graphql` with `nearVector`,
so `search()` builds query text rather than a JSON body. Nothing the caller
supplies is interpolated as a string: the collection name is validated in the
constructor and everything else in the query is a number.

**Two things answer 200 when they failed.** GraphQL always answers 200 and puts
the failure in an `errors` array, so `search()` reads the body rather than the
status. `POST /v1/batch/objects` answers 200 with a per-object `result.status`,
which means a dimension mismatch in one document of a batch looks like a
successful request. `add_batch()` checks every object and throws with the
server's message.

**Metadata** is stored as one JSON text property, so nested objects and arrays
come back as the value you put in rather than flattened to text the way Chroma
needs. The trade is that Weaviate's `where` filters cannot see inside it, which
costs nothing through this interface because it does not expose filtering.

`size()` asks the server (`Aggregate { … { meta { count } } }`), and a collection
that does not exist counts as empty. `clear()` deletes the collection; the next
write recreates it.

`docs/proofs/weaviate.md` has the probes those decisions came from, run against
`semitechnologies/weaviate:1.39.0`.

## Redis

```cpp
#include <tiny_agent/vectorstore/redis.hpp>

RedisVectorStore store{"redis://localhost:6379", "my_docs"};
```

Needs the search module: `redis/redis-stack-server`, Redis 8, or Redis Cloud
with search enabled. Plain Redis without the module answers the first write with
`ERR unknown command 'FT.CREATE'`.

The url takes the shapes people actually type. `localhost`, `localhost:6379`,
`redis://cache:6380/3` for a numbered database, `redis://alice:s3cret@cache:6379`
for an ACL user, `redis://s3cret@cache:6379` for a plain `requirepass` server.
Credentials in `RedisConfig` override anything in the url. `rediss://` throws at
construction: there is no TLS in this client, and saying so beats failing at the
handshake. Terminate TLS in front of it if you need it.

**No client library.** Redis speaks RESP over a raw TCP socket rather than HTTP,
so this adapter cannot ride on the httplib client the Qdrant and Chroma adapters
use. The protocol it needs is small: encode a command as an array of
length-prefixed bulk strings, parse the five RESP2 reply types back. That client
is about 280 lines of code at the top of the header, socket handling included. It
includes `<httplib.h>` for the platform work rather than duplicating it, because
that is where this codebase's winsock-versus-POSIX split already lives: on
Windows httplib includes `<winsock2.h>` and `<ws2tcpip.h>` and runs `WSAStartup`
from a static initialiser, on POSIX it pulls in `<netdb.h>` and `<sys/socket.h>`.
Everything after that is `getaddrinfo`, `connect`, `send`, `recv`, which are the
same calls on both.

**How it maps onto Redis.** One index per store, over hashes keyed
`<index_name>:<document_id>`:

```
FT.CREATE my_docs ON HASH PREFIX 1 my_docs: SCHEMA
  embedding VECTOR HNSW 6 TYPE FLOAT32 DIM 1536 DISTANCE_METRIC COSINE
```

Created on the first write and sized from the first embedding, like Qdrant's
collection. `RedisConfig` swaps `HNSW` for `FLAT` and `COSINE` for `L2` or `IP`.
Only the vector field is in the schema; `content` and `metadata` ride along as
ordinary hash fields, because `FT.SEARCH … RETURN` reads them straight off the
hash and indexing text nobody queries by only costs memory.

Documents go out as pipelined `HSET`s, so `add_batch` is one socket write and one
batch of replies rather than N round trips. The embedding is a raw little-endian
float32 blob, which is what the vector field expects, and it travels as a bulk
string argument, so its NUL and CR bytes never meet a parser.

Searches use the KNN form with the query vector passed as a parameter:

```
FT.SEARCH my_docs "*=>[KNN 4 @embedding $BLOB AS vector_score]"
  PARAMS 2 BLOB <blob> RETURN 3 content metadata vector_score
  SORTBY vector_score LIMIT 0 4 DIALECT 2
```

`DIALECT 2` is not optional: vector query syntax does not exist in dialect 1.
Passing the vector through `PARAMS` rather than splicing it into the query string
is what keeps binary bytes away from the query parser.

**Scores.** RediSearch reports a distance. Cosine and inner-product distances are
both `1 - similarity`, so the adapter inverts them and the number comes back
identical to what `FlatVectorStore` computes for the same vectors. L2 is a
squared euclidean distance with no upper bound, so it maps through `1/(1+d)`
instead, which preserves the ranking but is not comparable to a cosine score.

**Dimension mismatches.** A hash whose vector is the wrong length is accepted by
`HSET` and then silently dropped from the index, which looks exactly like a write
that worked until a search comes back short. `add()` checks the length against
the index and throws instead.

`size()` asks the server (`FT.SEARCH <index> * LIMIT 0 0`) rather than counting
locally. `clear()` deletes the documents by `SCAN` and `DEL` and then drops the
index, so it also cleans up hashes left behind by an index that was never
created; the next write recreates both.

## Milvus

```cpp
#include <tiny_agent/vectorstore/milvus.hpp>

MilvusVectorStore store{"http://localhost:19530", "my_docs",
                        MilvusConfig{.token = "…", .metric_type = "COSINE"}};
```

REST only, no SDK and no gRPC. Milvus multiplexes both onto port 19530 and serves
the v2 API under `/v2/vectordb`; port 9091 answers `/healthz` and nothing else.

**Ids are your own.** The primary key is declared `VarChar`, so the id you write
is the id Milvus holds and the id `search()` gives back. Qdrant and Weaviate
accept only UUIDs and their adapters hash around that; this one has nothing to
hash. The collection is created on the first write with the schema spelled out,
because Milvus's quick setup gives an `Int64` key and the `Bounded` consistency
level. The adapter asks for `Strong`, so a search issued right after a write sees
it.

**Writes go to `/entities/upsert`, not `/entities/insert`.** Insert does not look
at the primary key. Write `doc_a` twice and you have two rows, so re-indexing a
corpus doubles it and searches start returning both versions. Upsert replaces.

**Reads need the collection loaded.** A Milvus collection answers nothing until
it is in memory, and the load that follows creation is asynchronous, so a search
fired straight after the first write comes back `collection not loaded`. Both
reads wait for the load first and cache the result. Writes need no such thing,
which is why the failure only ever shows up on the read side. Nothing in Qdrant,
Chroma or Weaviate has an equivalent state.

**`size()` runs a `count(*)` query.** `/collections/get_stats` looks like the
call for this and gives the wrong answer: it counts sealed segments, so a
collection whose rows are still in memory reports zero. A collection that does
not exist counts as empty.

**Every failure arrives as HTTP 200** with a non-zero `code` in the body, on
every endpoint rather than just the batch path Weaviate has. Reading the status
alone turns a dimension mismatch into a silent success, so each call checks the
body and throws with the server's message.

**The metric is already a similarity.** Chroma and Weaviate report a distance and
their adapters return `1 - distance`. Under COSINE, the number Milvus labels
`distance` is the cosine similarity itself, and inverting it would reverse the
ranking, so it passes through untouched. `L2` is a real distance and gets
negated, which keeps the ordering exact.

**Metadata** is a JSON field, so nested objects and arrays come back as the value
you put in rather than flattened the way Chroma needs. The REST response
serialises the field to text and the adapter parses it back.

`clear()` drops the collection, and dropping one that is not there succeeds. The
next write recreates it.

`docs/proofs/milvus.md` has the probes those decisions came from, run against
`milvusdb/milvus:latest` at Milvus 3.0.0.

## Picking a backend at runtime

The concept resolves at compile time, which is what you want when the backend is
known. When it comes from a config file, wrap it:

```cpp
AnyVectorStore store = cfg.backend == "qdrant"
    ? AnyVectorStore{QdrantVectorStore{cfg.url, "docs"}}
    : AnyVectorStore{FlatVectorStore{}};

Retriever<MyEmbeddings, AnyVectorStore> retriever{std::move(store), embeddings, 4};
```

`AnyVectorStore` satisfies `vector_store` itself, so everything downstream is
unchanged.

## Writing an adapter

Implement the four methods against whatever you have. If it is an HTTP service,
`vectorstore/qdrant.hpp`, `vectorstore/chroma.hpp`, `vectorstore/weaviate.hpp`
and `vectorstore/milvus.hpp` are the pattern to copy: keep an `httplib::Client`,
and factor the request body and response parsing into static pure functions so
the wire format is testable without a server. That is what lets
`tests/test_vectorstore_remote.cpp`, `tests/test_vs_weaviate.cpp` and
`tests/test_vs_milvus.cpp` cover the adapters offline and run the same
round-trip against a live server when `QDRANT_URL`, `CHROMA_URL`,
`WEAVIATE_URL` or `MILVUS_URL` is set.

If it is not a REST service, `vectorstore/redis.hpp` is the pattern: the same
static pure functions for wire format and reply parsing, plus a small protocol
client above them that is also testable on its own. `tests/test_vs_redis.cpp`
asserts RESP encoding and every command the adapter can send with no server
present, then runs the live round-trip when `REDIS_URL` is set.

## Example

`examples/19_vector_store.cpp` indexes a small corpus and queries it through
`FlatVectorStore`, Qdrant, Chroma, Weaviate, Redis and `AnyVectorStore` in
one run. They
all return the same ranking and the same scores, which is the point of
normalizing the metric in the adapters.

```bash
./build/examples/19_vector_store
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
  WEAVIATE_URL=http://localhost:8080 \\
  REDIS_URL=redis://localhost:6379 ./build/examples/19_vector_store
```
