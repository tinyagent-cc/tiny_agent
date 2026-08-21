# Langfuse exporter, verified live

**Date:** 2026-08-21
**Langfuse:** 4.16.0 self-hosted (`docker.langfuse.com/langfuse/langfuse:4`, revision `12453aab35d43f1f5254571c95e456a03cf6a4ca`)
**Host:** macOS arm64, podman 5.7.1, podman-compose 1.5.0
**tiny_agent:** `chore/langfuse-live` off `4c8eeea`

`observability/langfuse.hpp` was written against Langfuse's documentation and had
never touched a running instance. This is that run: a real Langfuse on podman, a
two-span trace exported through `langfuse_exporter()`, and the ingested
observations read back out of the public API.

The exporter needed no changes. Endpoint, auth, content type, headers and the
whole attribute vocabulary were already what Langfuse reads. What changed is the
test: it used to assert that the POST did not throw, which proves the server
accepted bytes and nothing about what it stored.

## Standing up Langfuse

Adapted from [the upstream compose
file](https://github.com/langfuse/langfuse/blob/main/docker-compose.yml): every
container renamed `tacamp-lf-*`, host ports moved off the defaults, telemetry
off, and the `LANGFUSE_INIT_*` block filled in so the org, project and key pair
exist on first boot with no browser involved.

```yaml
# tacamp-lf/docker-compose.yml, trimmed to what differs from upstream
services:
  clickhouse:   { image: docker.io/clickhouse/clickhouse-server:25.12, container_name: tacamp-lf-clickhouse, ports: ["127.0.0.1:8127:8123"] }
  minio:        { image: cgr.dev/chainguard/minio,                     container_name: tacamp-lf-minio,      ports: ["127.0.0.1:9097:9000"] }
  redis:        { image: docker.io/redis:7,                            container_name: tacamp-lf-redis,      ports: ["127.0.0.1:6389:6379"] }
  postgres:     { image: docker.io/postgres:17,                        container_name: tacamp-lf-postgres,   ports: ["127.0.0.1:5442:5432"] }
  langfuse-worker: { image: docker.langfuse.com/langfuse/langfuse-worker:4, container_name: tacamp-lf-worker }
  langfuse-web:
    image: docker.langfuse.com/langfuse/langfuse:4
    container_name: tacamp-lf-web
    ports: ["127.0.0.1:3007:3000"]
    environment:
      NEXTAUTH_URL: http://localhost:3007
      LANGFUSE_INIT_ORG_ID: tacamp
      LANGFUSE_INIT_ORG_NAME: Tiny Agent Camp
      LANGFUSE_INIT_PROJECT_ID: tacamp-tiny-agent
      LANGFUSE_INIT_PROJECT_NAME: tiny_agent
      LANGFUSE_INIT_PROJECT_PUBLIC_KEY: pk-lf-tacamp-0000-0000-0000-000000000001
      LANGFUSE_INIT_PROJECT_SECRET_KEY: sk-lf-tacamp-0000-0000-0000-000000000001
      LANGFUSE_INIT_USER_EMAIL: dev@tacamp.local
      LANGFUSE_INIT_USER_NAME: tacamp
      LANGFUSE_INIT_USER_PASSWORD: tacamp-dev-password
```

The `LANGFUSE_INIT_*` variables belong on `langfuse-web` only, and they apply on
first boot against an empty Postgres. The rest of the stack is upstream's
defaults with the CHANGEME placeholders filled in.

```bash
podman-compose -p tacamp-lf up -d
```

```
tacamp-lf-clickhouse  Up 11 seconds (healthy)  127.0.0.1:8127->8123/tcp
tacamp-lf-minio       Up 11 seconds (healthy)  127.0.0.1:9097->9000/tcp
tacamp-lf-redis       Up 11 seconds (healthy)  127.0.0.1:6389->6379/tcp
tacamp-lf-postgres    Up 10 seconds (healthy)  127.0.0.1:5442->5432/tcp
tacamp-lf-worker      Up 2 seconds             3030/tcp
tacamp-lf-web         Up 2 seconds             127.0.0.1:3007->3000/tcp
```

The bootstrapped keys work straight away:

```bash
curl -s -u "pk-lf-tacamp-…:sk-lf-tacamp-…" http://localhost:3007/api/public/projects
```

```json
{"data":[{"id":"tacamp-tiny-agent","name":"tiny_agent","organization":{"id":"tacamp","name":"Tiny Agent Camp"},"metadata":{}}]}
```

## The live test

```bash
cmake --preset default && cmake --build build -j8
cd build/tests
LANGFUSE_BASE_URL=http://localhost:3007 \
LANGFUSE_PUBLIC_KEY=pk-lf-tacamp-0000-0000-0000-000000000001 \
LANGFUSE_SECRET_KEY=sk-lf-tacamp-0000-0000-0000-000000000001 \
  ./test_tracing -tc="live Langfuse export against configured credentials" -s
```

The span pair is an `agent` root with a failing `llm` child under it, so one
export covers parenting, the observation-type vocabulary, the token counts and
the error path. Raw output:

```
[doctest] doctest version is "2.5.1"
[doctest] run with "--help" for options
===============================================================================
tests/test_tracing.cpp:590:
TEST CASE:  live Langfuse export against configured credentials

tests/test_tracing.cpp:636: SUCCESS: REQUIRE_NOTHROW( exporter->export_spans({root, child}) ) didn't throw!

tests/test_tracing.cpp:637: MESSAGE: langfuse trace id: a435bfd9c3df3b1291a81cce93577f9a

tests/test_tracing.cpp:644: SUCCESS: REQUIRE( body.contains("data") ) is correct!
  values: REQUIRE( true )

tests/test_tracing.cpp:645: SUCCESS: REQUIRE( body["data"].size() == 2 ) is correct!
  values: REQUIRE( 2 == 2 )

tests/test_tracing.cpp:648: SUCCESS: REQUIRE_FALSE( gen.is_null() ) is correct!
  values: REQUIRE_FALSE( false )

tests/test_tracing.cpp:649: SUCCESS: CHECK( gen.at("type") == "GENERATION" ) is correct!
  values: CHECK( "GENERATION" == GENERATION )

tests/test_tracing.cpp:650: SUCCESS: CHECK( gen.at("level") == "ERROR" ) is correct!
  values: CHECK( "ERROR" == ERROR )

tests/test_tracing.cpp:651: SUCCESS: CHECK( gen.at("statusMessage") == "rate limited" ) is correct!
  values: CHECK( "rate limited" == rate limited )

tests/test_tracing.cpp:652: SUCCESS: CHECK( gen.at("model") == "gpt-4o-mini" ) is correct!
  values: CHECK( "gpt-4o-mini" == gpt-4o-mini )

tests/test_tracing.cpp:653: SUCCESS: CHECK( gen.at("input") == R"([{"role":"user","content":"hi"}])" ) is correct!
  values: CHECK( "[{\"role\":\"user\",\"content\":\"hi\"}]" == [{"role":"user","content":"hi"}] )

tests/test_tracing.cpp:654: SUCCESS: CHECK( gen.at("output") == "Hello!" ) is correct!
  values: CHECK( "Hello!" == Hello! )

tests/test_tracing.cpp:655: SUCCESS: CHECK( gen.at("usageDetails").at("input") == 12 ) is correct!
  values: CHECK( 12 == 12 )

tests/test_tracing.cpp:656: SUCCESS: CHECK( gen.at("usageDetails").at("output") == 3 ) is correct!
  values: CHECK( 3 == 3 )

tests/test_tracing.cpp:657: SUCCESS: CHECK( gen.at("sessionId") == "tiny-agent-live-session" ) is correct!
  values: CHECK( "tiny-agent-live-session" == tiny-agent-live-session )

tests/test_tracing.cpp:658: SUCCESS: CHECK( gen.at("userId") == "tiny-agent-live-user" ) is correct!
  values: CHECK( "tiny-agent-live-user" == tiny-agent-live-user )

tests/test_tracing.cpp:659: SUCCESS: CHECK( gen.at("traceName") == "live-roundtrip" ) is correct!
  values: CHECK( "live-roundtrip" == live-roundtrip )

tests/test_tracing.cpp:662: SUCCESS: REQUIRE_FALSE( agent.is_null() ) is correct!
  values: REQUIRE_FALSE( false )

tests/test_tracing.cpp:663: SUCCESS: CHECK( agent.at("type") == "AGENT" ) is correct!
  values: CHECK( "AGENT" == AGENT )

tests/test_tracing.cpp:664: SUCCESS: CHECK( agent.at("isRootObservation") == true ) is correct!
  values: CHECK( true == true )

tests/test_tracing.cpp:665: SUCCESS: CHECK( gen.at("parentObservationId") == agent.at("id") ) is correct!
  values: CHECK( "046d2ceaeaaa421e" == "046d2ceaeaaa421e" )

===============================================================================
[doctest] test cases:  1 |  1 passed | 0 failed | 34 skipped
[doctest] assertions: 19 | 19 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## What Langfuse stored

```bash
curl -s -u "pk-lf-tacamp-…:sk-lf-tacamp-…" \
  "http://localhost:3007/api/public/v2/observations?traceId=a435bfd9c3df3b1291a81cce93577f9a&fields=core,basic,time,io,model,usage,metrics,trace_context"
```

```json
{
    "data": [
        {
            "id": "f41173c41454821a",
            "traceId": "a435bfd9c3df3b1291a81cce93577f9a",
            "startTime": "2026-08-21T17:36:45.362Z",
            "endTime": "2026-08-21T17:36:46.262Z",
            "projectId": "tacamp-tiny-agent",
            "parentObservationId": "046d2ceaeaaa421e",
            "type": "GENERATION",
            "name": "live-chat",
            "level": "ERROR",
            "statusMessage": "rate limited",
            "environment": "default",
            "input": "[{\"role\":\"user\",\"content\":\"hi\"}]",
            "output": "Hello!",
            "model": "gpt-4o-mini",
            "internalModelId": "clyrjp56f0000t0mzapoocd7u",
            "usageDetails": { "input": 12, "output": 3, "total": 15 },
            "inputUsage": 12,
            "outputUsage": 3,
            "totalUsage": 15,
            "latency": 0.9,
            "userId": "tiny-agent-live-user",
            "sessionId": "tiny-agent-live-session",
            "isRootObservation": false,
            "traceName": "live-roundtrip"
        },
        {
            "id": "046d2ceaeaaa421e",
            "traceId": "a435bfd9c3df3b1291a81cce93577f9a",
            "startTime": "2026-08-21T17:36:45.362Z",
            "endTime": "2026-08-21T17:36:46.862Z",
            "projectId": "tacamp-tiny-agent",
            "parentObservationId": null,
            "type": "AGENT",
            "name": "live-roundtrip",
            "level": "DEFAULT",
            "statusMessage": "",
            "environment": "default",
            "input": "{\"question\":\"round trip\"}",
            "output": "done",
            "model": "",
            "usageDetails": {},
            "latency": 1.5,
            "userId": "tiny-agent-live-user",
            "sessionId": "tiny-agent-live-session",
            "isRootObservation": true,
            "traceName": "live-roundtrip"
        }
    ],
    "meta": {}
}
```

Every field the exporter is responsible for arrived where Langfuse's UI reads
it. `internalModelId` is Langfuse matching `gpt-4o-mini` against its own model
table off the model name we sent, which is the pricing path working.

## Notes from the run

**Reading traces back needs the v2 API.** A self-hosted v4 runs in `events_only`
write mode, and the older `GET /api/public/traces` answers with

```json
{"message":"This endpoint is not available on deployments running in Langfuse v4 events_only mode. Learn more about Langfuse v4 at: https://langfuse.com/docs/v4"}
```

`GET /api/public/v2/observations?traceId=…` is the endpoint that works, and the
`fields` parameter gates what comes back: `traceName`, `userId` and `sessionId`
need `trace_context` in the list or they are simply absent from the response
rather than null.

**OTLP/HTTP JSON is a first-class content type**, not a tolerated one. The
handler in `web/src/pages/api/public/otel/v1/traces/index.ts` branches on
`application/json` and `application/x-protobuf` and rejects anything else with a
400, which settles the question the exporter was built on.

**`x-langfuse-ingestion-version: 4` earns its place.** Without it OTLP data still
ingests, but on the delayed compatibility path; with it the spans were queryable
about two seconds after the POST returned.

**Error status needs no extra attribute.** Langfuse derives `level: ERROR` from
OTLP `status.code == 2` and takes `statusMessage` from `status.message`, both of
which `to_otlp_json()` already writes.

**Ingestion is asynchronous**, so the test polls for the observations rather than
reading once. Two seconds was typical here; the poll budget is 60.

## Offline suite

The live test skips itself when the key pair is absent, so the ordinary run is
unchanged:

```
$ ctest --output-on-failure
      Start 16: test_tracing
16/20 Test #16: test_tracing .....................   Passed    0.02 sec
…
100% tests passed, 0 tests failed out of 20

Total Test time (real) =   6.35 sec
```

## Teardown

```bash
podman-compose -p tacamp-lf down -v
```
