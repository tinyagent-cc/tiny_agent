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

Implement the four methods against whatever you have. If it is a REST service,
`vectorstore/qdrant.hpp` and `vectorstore/chroma.hpp` are the pattern to copy:
keep an `httplib::Client`, and factor the request body and response parsing into
static pure functions so the wire format is testable without a server. That is
what lets `tests/test_vectorstore_remote.cpp` cover both adapters offline and
run the same round-trip against a live server when `QDRANT_URL` or `CHROMA_URL`
is set.

## Example

`examples/19_vector_store.cpp` indexes a small corpus and queries it through
`FlatVectorStore`, Qdrant, Chroma and `AnyVectorStore` in one run. All four
return the same ranking and the same scores, which is the point of normalizing
the metric in the adapters.

```bash
./build/examples/19_vector_store
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
  ./build/examples/19_vector_store
```
