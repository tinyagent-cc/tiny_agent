# Security

## Supported versions

Only the latest minor release gets security fixes. tiny_agent is pre-1.0 and
header-only: there is no long-term branch to backport to, so upgrading to the
newest tag is the fix.

| Version | Supported |
|---|---|
| latest minor (0.x, most recent) | yes |
| anything older | no |

## Reporting a vulnerability

Do not open a public issue for a security problem. Use GitHub's private
reporting instead: **Security → Report a vulnerability** on
[github.com/rhajamor/tiny_agent](https://github.com/rhajamor/tiny_agent/security/advisories/new).

Include what the issue affects (a provider adapter, the SSE parser, the MCP
transport, the CMake port, …), how to reproduce it, and the impact you expect.
A working proof of concept speeds up triage but is not required to file.

Expect an acknowledgment within a few days. There is no fixed SLA yet — this is
a small project, not a vendor with a support contract — but a confirmed report
gets a fix or a mitigation before it gets a public writeup.

## Scope

In scope: the library headers, the MCP client, the provider adapters, the
vcpkg port. Out of scope: vulnerabilities in a model provider's own API,
in vcpkg or its other ports, or in a service you pointed a vector-store or
observability adapter at (Qdrant, Chroma, Phoenix, Langfuse, your own
llama.cpp server) — report those upstream.
