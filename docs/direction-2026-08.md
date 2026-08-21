# tiny_agent direction, August 2026

Decision doc. Where to take tiny_agent, based on a read of the full codebase on 2026-08-21 and a competitive sweep of the C++ and edge-agent landscape the same day. Success, per Riadh, is all four rungs of a ladder: something autonomous running on his own Pi 5, open-source traction, a plausible commercial endgame, and a flagship project for the career story. The recommendation below is chosen because it stacks those rungs instead of trading them off.

## Current state

The repo is 5,173 lines of headers and covers more ground than that number suggests.

What is genuinely there, verified by reading the code:

- Modern C++20 design: `is_chat` and `is_embedding` concepts, a primary `LLMModel<Provider, Kind>` template fully specialized per provider, and `ChatVariant` dispatch built on `std::variant` and `std::visit`. No virtual functions, no heap allocation in dispatch (core/model.hpp).
- A complete ReAct agent: `AgentExecutor<deep_agent_tag, LLM>` with a tool registry, JSON-schema-validated tool calls, error-isolated tool execution, a middleware chain, multi-turn history, and sub-agent delegation via `as_tool` and `agent_as_tool` (agent.hpp).
- 12 built-in middleware: summarize, trim-history, PII, retry, model fallback, context editing, call limits, logging, and more. This is close to LangChain middleware parity for the essentials.
- MCP over both stdio and HTTP transports (mcp/).
- An Agent Skills loader following the agentskills.io SKILL.md spec (skills/). Rare in any language outside the Anthropic ecosystem, and unique in C++.
- Seven providers (OpenAI, Anthropic, Gemini, Cohere, Mistral, VoyageAI, plus local), embeddings, three vector stores (flat, hnswlib, qdrant), a retriever, and batch.
- 188 doctest cases across 13 files and 24 examples.

Honest gaps, also from the code:

- `stream()` is fake: it invokes synchronously and fires the callback once with the whole response (model.hpp:188, agent.hpp:219).
- `batch()` is a sequential loop, and there is no async story at all.
- One agent strategy exists (`deep_agent_tag`); the strategy-tag architecture is ready for more but nothing else is implemented.
- Local model support is 26 lines of aliasing: `local::ollama/llamacpp/vllm` just point the OpenAI-compatible provider at localhost ports (providers/local.hpp). Everything goes over HTTP; there is no in-process llama.cpp binding.
- Nothing is actually Pi-specific despite the README mentioning Raspberry Pi builds.
- No persistence beyond in-memory history, no scheduler, nothing that would make an agent autonomous rather than invoked.
- **No LICENSE file.** Legally the repo is all-rights-reserved today. This matters below.

GitHub remote (`rhajamor/tiny_agent`): zero stars, zero forks, zero issues, single `main` branch identical to local, last push 2026-05-12. Clean slate, no community constraints on repositioning.

## Landscape

Full sweep run 2026-08-21, sources fetched that day.

**C++ agent frameworks.** llama.cpp ships OpenAI-style function calling in llama-server but explicitly leaves the agent loop to the caller ([function-calling.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/function-calling.md)). The two live competitors are both handicapped: [ClickHouse/ai-sdk-cpp](https://github.com/ClickHouse/ai-sdk-cpp) (~182 stars) has multi-step tool calling but no Gemini, no MCP, no local-model story, and is not header-only; [RunEdgeAI/agents.cpp](https://github.com/RunEdgeAI/agents.cpp) (~99 stars) is the closest feature match but is Bazel-only, ships under an "evaluation license," and paywalls MCP in a Pro tier, which poisons it for OSS adoption. [OpenSparX/MasterAgent](https://github.com/OpenSparX/MasterAgent) (423 stars) is an on-device agent kernel for Qualcomm automotive NPUs, a product rather than an embeddable library. The graveyard is instructive: humanus.cpp, instinct.cpp, langgraph-cpp, and mentals-ai are all dead or tiny. LangChain-for-C++ has been tried and abandoned repeatedly; a small composable library has not.

**Edge agent runtimes.** [Cactus](https://github.com/cactus-compute/cactus) (5.9k stars, YC S25) is the momentum leader for on-device inference and just shipped [Needle2](https://cactuscompute.com/needle), a 14MB agentic model doing 500 tok/s on a Raspberry Pi, launched 2026-08-10 with 534 HN points. That launch validates Pi-class agent demand loudly, and Cactus is an inference engine with thin orchestration, not an agent framework. NVIDIA's [Jetson AI Lab](https://www.jetson-ai-lab.com/) is Python-stack. [Home Assistant](https://www.home-assistant.io/blog/categories/announcements/) owns smart-home agent UX end to end in Python; it is a platform to integrate with, not to compete with.

**Robotics.** ROS 2 agent work is Python-owned and funded: NASA JPL's [ROSA](https://github.com/nasa-jpl/rosa) (1.6k stars), RobotecAI's [RAI](https://github.com/RobotecAI/rai) (571 stars). The C++ side is inference-only ([llama_ros](https://github.com/mgonzs13/llama_ros), 262 stars). Nobody has put a C++ agent layer on top, but the incumbents are strong and the integration lift is heavy.

**The gap.** No live, permissively-licensed, header-only, multi-provider C++ agent framework with MCP exists. tiny_agent's feature set is already that thing, minus the license file. Inference is a solved and crowded layer; orchestration in C++ is the empty one.

## Candidate directions

**1. Lightweight C++ agent library with an edge narrative.** Position: "the header-only C++20 agent layer for llama.cpp and Ollama, small enough for a Pi, MCP included, MIT licensed." Market pull: validated gap, both live rivals handicapped, and the Needle2 wave needs exactly this orchestration layer. Effort to credibility: low, the codebase is roughly 80% there; the work is streaming, CI, packaging, license, and a benchmark story, not new architecture. Fit: exact. Risk: C++ agent developers are a niche audience, and a bigger player (Cactus, ClickHouse) could decide to own the layer.

**2. Embedded autonomous agent runtime for Pi-class hardware.** A daemon built on the library: scheduling, persistence, sensor and GPIO tools, an autonomy loop, config. This is the "autonomous systems on a Pi" vision directly. Pull: demand-validated by the same signals. Effort: high; a runtime needs a product surface (packaging, safety, updates, observability) the library does not, and the hard layer underneath (inference) is owned by Cactus and llama.cpp, so the runtime only wins as orchestration on top. Fit: good, but it needs the library to be credible first. Risk: building a product for an audience not yet met.

**3. Robotics and home-automation runtime (ROS 2 or Home Assistant).** Pull exists, but ROSA and RAI are funded Python incumbents with conference presence, and ROS 2 integration is the heaviest lift of the three. Fit: weakest; nothing in the codebase touches ROS. Verdict: not a direction. Keep home automation as demo material for direction 1 and revisit ROS only if inbound interest appears.

A fourth option, going commercial-first the way agents.cpp and MasterAgent have, was considered and rejected: their licenses are precisely what makes them beatable, and tiny_agent has no user base to monetize yet.

## Recommendation

**Direction 1, committed: the header-only C++ agent layer for local models, marketed through a Raspberry Pi demo. Direction 2 becomes the second act once the library has users, and its first increment (the Pi demo daemon) doubles as direction 1's marketing.**

This is the only direction that stacks all four success rungs in order: the Pi 5 demo is the personal-autonomy rung and the launch asset; the library is the OSS-traction rung; the commercial rung stays open later via open-core tooling or fleet management once there are users (agents.cpp paywalling MCP proves appetite exists, and tiny_agent wins today by keeping MCP free); the whole arc, a solo modern-C++ framework with a live edge demo, is a strong flagship for the career story.

The positioning sentence for the README: **tiny_agent is the header-only C++20 agent framework: multi-provider, MCP built in, MIT licensed, and small enough to run your agent on a Raspberry Pi.**

## Milestones

**M1, credible release (v0.3).** Everything a skeptical C++ dev checks in the first two minutes. Add the MIT license (blocking, the repo currently has none, so every differentiation claim is void until this lands). Implement real SSE streaming for at least OpenAI-compatible and Anthropic providers, replacing the fake `stream()`. GitHub Actions CI with an arm64 job proving the Pi build claim. Rewrite the README around the positioning sentence, with a benchmark table (binary size, RSS, time-to-first-token against ai-sdk-cpp and agents.cpp on identical tasks). Submit a vcpkg port. Build first: license, then streaming, since the demo in M2 is unconvincing without token streaming.

**M2, the Pi flagship demo.** A tiny_agent binary on the Pi 5 running against llama.cpp with a small tool-calling model (Qwen-class today, Needle when its weights are usable), doing a genuinely autonomous job: watching something real (sensors, home network, calendar), deciding, and acting, with Telegram as the notification channel. Record it with the existing demo-video pipeline, then launch: HN Show, r/cpp, r/LocalLLaMA, riding the on-device agent wave while it is cresting. This demo is also the seed of direction 2's daemon.

**M3, ecosystem hooks and the commercial decision.** MCP examples against real public servers, a small skills gallery exercising the agentskills.io loader, and an optional in-process llama.cpp binding behind a vcpkg feature so "edge" stops meaning "HTTP to localhost." Then read the traction (stars, issues, inbound) and decide the commercial angle: open-core pro tooling, paid fleet management for direction 2's daemon, or deliberately staying OSS as a career asset. That decision has no deadline; making it before M2's launch data exists would be guessing.

## Sources

Codebase claims: files read 2026-08-21 in `~/git/tiny_agent_cpp` (core/model.hpp, agent.hpp, providers/local.hpp, skills/skill.hpp, tests/, README.md, vcpkg.json, full file listing). Remote state: `gh repo view rhajamor/tiny_agent`, same date. Market claims: URLs inline above, all fetched 2026-08-21.
