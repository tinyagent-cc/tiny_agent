# Launch kit

Not linked from the site. Copy for the ProductHunt post.

## Tagline

**An AI agent framework that fits on a Raspberry Pi** (49 characters)

It leads with the hardware instead of the language, so the claim lands on
someone who has never written C++. The size story is the differentiator and it
is measured, not asserted.

### Alternates

- Header-only C++20 agent framework for the edge (46)
- Tool-using AI agents in C++, offline on a Pi (44)
- The agent loop for C++, in 7 MB (31)

## First comment

Hey PH 👋

Same wall every time I tried to build an agent in C++: llama.cpp and Ollama run
the model fine, and llama.cpp's own docs tell you the agent loop is your
problem. So I wrote the loop.

tiny_agent is header-only C++20. One include, no runtime, no virtual dispatch in
the dispatch path. Seven providers, MCP over stdio and HTTP with nothing behind
a paid tier, sub-agents that are just tools, fourteen middlewares, SSE
streaming, vector stores, tracing to Phoenix or Langfuse.

The part I actually care about is that it runs on a Pi 5. The streaming example
is a 7.4 MB stripped binary and peaks at 2.0 MB RSS while streaming from a local
llama.cpp server. Measured on the board, raw output in the repo.

Known gap: one example won't build on g++ 11.4. MIT licensed, so vendor it and
ship it.
