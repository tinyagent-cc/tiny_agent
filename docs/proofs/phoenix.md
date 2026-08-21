# Arize Phoenix: live verification

**Date:** 2026-08-21
**Backend:** `docker.io/arizephoenix/phoenix:latest`, `/arize_phoenix_version` reports `20.3.0`
**Image digest:** `sha256:22358dc39de9aa02d47afdd6ce659747f511bc887ebf927d74d695e34ec52c75`
**Runtime:** podman on macOS (podman-machine-default, libkrun)

Evidence that `obs::phoenix_exporter` (`include/tiny_agent/observability/phoenix.hpp`)
actually delivers spans to a running Phoenix instance, beyond the offline OTLP
payload-shaping tests in `tests/test_tracing.cpp`.

## Start the container

```
podman run -d --name tacamp-phoenix -p 6006:6006 docker.io/arizephoenix/phoenix:latest
```

```
555007775c356adfe8c6e836f6e1b72195256a3066d2520bb55af003a208707e
```

Health check:

```
curl -s http://localhost:6006/healthz
curl -s http://localhost:6006/arize_phoenix_version
```

```
OK
20.3.0
```

## Configure and build

Same tree as `qdrant.md`, one `cmake --preset default` and one
`cmake --build build -j 8`, both exit 0.

## ctest: offline (no env vars)

`test_tracing` runs with no `PHOENIX_BASE_URL` set; its live-export case
returns via `REQUIRE_FALSE(base)`.

```
      Start 16: test_tracing
16/20 Test #16: test_tracing .....................   Passed    0.39 sec
...
100% tests passed, 0 tests failed out of 20
Total Test time (real) =   6.47 sec
```

## ctest: live (PHOENIX_BASE_URL set)

```
QDRANT_URL=http://localhost:6333 CHROMA_URL=http://localhost:8000 \
PHOENIX_BASE_URL=http://localhost:6006 \
ctest --output-on-failure --timeout 120
```

```
      Start 16: test_tracing .....................   Passed    0.02 sec
...
100% tests passed, 0 tests failed out of 20
Total Test time (real) =   1.50 sec
```

The Phoenix case isolated with doctest's own filter so the assertion is
visible, run alongside the OTLP and Langfuse cases in the same binary (both
correctly skip: no `TINY_AGENT_OTLP_ENDPOINT` or Langfuse keys were set):

```
PHOENIX_BASE_URL=http://localhost:6006 \
./tests/test_tracing --success -tc="live*"
```

```
===============================================================================
/tests/test_tracing.cpp:520:
TEST CASE:  live OTLP export against a configured collector

/tests/test_tracing.cpp:525: SUCCESS: REQUIRE_FALSE( endpoint ) is correct!
  values: REQUIRE_FALSE( nullptr )

===============================================================================
/tests/test_tracing.cpp:533:
TEST CASE:  live Phoenix export against a running instance

/tests/test_tracing.cpp:547: SUCCESS: REQUIRE_NOTHROW( exporter->export_spans({s}) ) didn't throw!

===============================================================================
/tests/test_tracing.cpp:550:
TEST CASE:  live Langfuse export against configured credentials

/tests/test_tracing.cpp:555: SUCCESS: REQUIRE_FALSE( configured ) is correct!
  values: REQUIRE_FALSE( false )

===============================================================================
[doctest] test cases: 3 | 3 passed | 0 failed | 32 skipped
[doctest] assertions: 3 | 3 passed | 0 failed |
[doctest] Status: SUCCESS!
```

`REQUIRE_NOTHROW` on the export call only proves the client thinks the POST
succeeded. To prove the span actually landed, the exporter's config sets
`project_name = "tiny-agent-test"` (see `test_tracing.cpp:542`), and after the
run that project exists server-side:

```
curl -s "http://localhost:6006/v1/projects"
```

```json
{"data":[{"name":"tiny-agent-test","description":null,"id":"UHJvamVjdDoy"},{"name":"default","description":"Default project","id":"UHJvamVjdDox"}],"next_cursor":null}
```

Phoenix only creates a project when it receives a span for it, so
`tiny-agent-test` showing up here is the server-side confirmation the
exporter's own `REQUIRE_NOTHROW` can't give.

## Example: `examples/18_tracing`

```
PHOENIX_BASE_URL=http://localhost:6006 ./build/examples/18_tracing
```

```
exporting to Phoenix at http://localhost:6006
set OPENAI_API_KEY, or OLLAMA_BASE_URL for a local model
```

Exit code 1. The example picks the Phoenix exporter correctly from the
`PHOENIX_BASE_URL` env var, which is the part that touches this integration,
but it also needs a live LLM to actually run the agent and produce a trace,
and neither `OPENAI_API_KEY` nor a local Ollama server was available in this
environment. The exporter-selection logic this example exists to demonstrate
ran; the agent turn past it did not. `test_tracing`'s live Phoenix case above
is what carries the actual span-delivery proof for this integration.

## Teardown

```
podman rm -f tacamp-phoenix
```

Confirmed absent afterward, see `docs/integrations.md` for the final
`podman ps -a` across all three containers.
