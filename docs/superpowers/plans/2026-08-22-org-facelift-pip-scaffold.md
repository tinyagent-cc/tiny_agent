# Org Face-lift + Pip Scaffold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make github.com/tinyagent-cc read as a real project to a stranger in one screen, and scaffold the `pip` repo the firmware plan will fill.

**Architecture:** Pure content and GitHub-metadata work: a `.github` profile repo, org/repo descriptions via `gh`, cross-links between the two existing repos, and a new `pip` repo holding README, protocol, and license only.

**Tech Stack:** git, `gh` CLI, Markdown.

**Spec:** `docs/superpowers/specs/2026-08-22-pip-companion-design.md` (Track 4 and the Pip repo scaffold from Track 1).

## Global Constraints

- Org: `tinyagent-cc`. Site: `https://tinyagent.cc`. Brand voice: plain, concrete, no hype adjectives.
- Prose a human will read follows `~/.agents/skills/humanize/SKILL.md` (sweep with `ai-tells.md`) and `~/git/mystaff/.claude/skills/posture/SKILL.md` (sweep with `tells.md`). Read both files before writing README prose.
- Never commit WiFi credentials or tokens; `config.example.h` pattern only.
- Every commit ends with the two-line trailer:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01H7hNgsKhpDyCMmrMbSLSfi`
- Do not force-push anywhere; do not touch repos outside `tinyagent-cc`.

---

### Task 1: Org profile repo

**Files:**
- Create: `<scratchpad>/ta-dotgithub/profile/README.md` (new repo `tinyagent-cc/.github`)

**Interfaces:**
- Produces: public org profile page at github.com/tinyagent-cc.

- [ ] **Step 1: Create the repo content locally**

Work in the session scratchpad. Write `profile/README.md`:

```markdown
<p align="center">
  <img src="https://raw.githubusercontent.com/tinyagent-cc/tiny_agent/main/site/assets/logo.svg" alt="tinyagent" width="72">
</p>

# tinyagent

C++ agents small enough for the hardware you already own. An agent loop in a
7 MB binary and 2 MB of RSS, header-only, running against llama.cpp on a
Raspberry Pi.

## [tiny_agent](https://github.com/tinyagent-cc/tiny_agent)

The framework: DeepAgent sub-agents, middleware, MCP, SSE streaming, RAG
vector stores, tracing. C++20, header-only, no framework runtime.

```cpp
auto agent = tiny_agent::agents::DeepAgent(init_chat_model("ollama:qwen3:0.6b"));
```

## [rete_cpp](https://github.com/tinyagent-cc/rete_cpp)

The reflex layer: a header-only C++17 Rete rules engine. Paired with
tiny_agent it answers the easy cases in microseconds with zero tokens, and
vetoes bad tool calls deterministically; alone it is a complete expert-system
engine with agenda, negation, and refraction.

## pip (in progress)

The proof: a desk companion with a face. Raspberry Pi Pico 2 W body,
tiny_agent brain, rete_cpp reflexes. Reflexes are rules; judgment is an
agent.

---

Site and benchmarks: [tinyagent.cc](https://tinyagent.cc) · MIT licensed
```

Before committing, verify the logo path actually exists:
`curl -sI https://raw.githubusercontent.com/tinyagent-cc/tiny_agent/main/site/assets/logo.svg | head -1` — if not 200, list `site/` in the tiny_agent repo and use the real asset path (there is a favicon/logo used by the live site; `grep -o 'assets/[a-z.-]*' ~/git/tiny_agent_cpp/site/index.html | sort -u` finds it). If only a PNG exists, use it with the same width.

- [ ] **Step 2: Create the repo and push**

```bash
cd <scratchpad>/ta-dotgithub
git init -b main && git add profile/README.md
git commit -m "docs: org profile README

<trailer>"
gh repo create tinyagent-cc/.github --public --source . --push \
  --description "Org profile for tinyagent"
```

- [ ] **Step 3: Verify render**

Run: `curl -s https://github.com/tinyagent-cc | grep -c 'reflex layer'`
Expected: at least 1. If 0, wait 30s and retry once (GitHub caches).

### Task 2: Org and repo metadata

**Files:** none (GitHub API state only).

**Interfaces:**
- Consumes: nothing.
- Produces: org description/website; rete_cpp description/homepage.

- [ ] **Step 1: Set org metadata**

```bash
gh api -X PATCH /orgs/tinyagent-cc \
  -f description="C++ agents small enough for the hardware you already own" \
  -f blog="https://tinyagent.cc"
```

If this 403s (token lacks `admin:org`), report it as a Riadh UI task with the exact strings; do not retry with other tokens.

- [ ] **Step 2: Set rete_cpp metadata**

```bash
gh repo edit tinyagent-cc/rete_cpp \
  --description "Header-only C++17 Rete rules engine — the reflex layer of the tinyagent stack" \
  --homepage "https://tinyagent.cc"
```

- [ ] **Step 3: Verify**

Run: `gh repo view tinyagent-cc/rete_cpp --json description -q .description`
Expected: the string set above.

### Task 3: Cross-links in the two READMEs

**Files:**
- Modify: `~/git/rete_cpp/README.md` (top section)
- Modify: `~/git/tiny_agent_cpp/README.md` (one pointer line in the integrations/ecosystem area)

**Interfaces:**
- Consumes: repo descriptions from Task 2 (wording consistency).
- Produces: bidirectional links a visitor can follow.

- [ ] **Step 1: Check rete_cpp's remote points at the org**

Run: `git -C ~/git/rete_cpp remote get-url origin`
Expected: contains `tinyagent-cc/rete_cpp`. If it still says `rhajamor/`, run
`git -C ~/git/rete_cpp remote set-url origin https://github.com/tinyagent-cc/rete_cpp.git`.

- [ ] **Step 2: Add positioning to rete_cpp README**

Directly under the existing `# RETE Expert System in Modern C++` heading, insert:

```markdown
Part of [tinyagent](https://github.com/tinyagent-cc): rete_cpp is the
stack's reflex layer. [tiny_agent](https://github.com/tinyagent-cc/tiny_agent)'s
`middleware/reflex.hpp` uses it to answer easy cases in microseconds without
a model call and to veto bad tool calls deterministically. rete_cpp itself
has no dependency on tiny_agent and works standalone.
```

Sweep the paragraph against `ai-tells.md` before committing.

- [ ] **Step 3: Add the pointer in tiny_agent README**

Find the integrations/ecosystem section (`grep -n 'integrations' ~/git/tiny_agent_cpp/README.md`); add one row/line:

```markdown
- **Reflexes and guardrails**: pair with [rete_cpp](https://github.com/tinyagent-cc/rete_cpp) via `middleware/reflex.hpp` — rules answer the easy cases in microseconds, and veto bad tool calls before dispatch. (Landing with the reflex-middleware plan.)
```

Drop the parenthetical once Plan 2 merges, if this task runs after it.

- [ ] **Step 4: Commit and push both**

```bash
git -C ~/git/rete_cpp add README.md && git -C ~/git/rete_cpp commit -m "docs: position rete_cpp inside the tinyagent stack

<trailer>" && git -C ~/git/rete_cpp push
git -C ~/git/tiny_agent_cpp add README.md && git -C ~/git/tiny_agent_cpp commit -m "docs: point to rete_cpp reflex pairing

<trailer>" && git -C ~/git/tiny_agent_cpp push
```

### Task 4: Pip repo scaffold

**Files:**
- Create (new repo `tinyagent-cc/pip`, local clone at `~/git/pip`):
  `README.md`, `PROTOCOL.md`, `LICENSE`, `.gitignore`

**Interfaces:**
- Produces: the repo the firmware plan (Plan 3) fills; `PROTOCOL.md` v0 that Plan 2's Pip-brain rules and Plan 3's firmware both implement.

- [ ] **Step 1: README**

```markdown
# Pip

A desk companion with a face. Pico 2 W body, tiny_agent brain, rete_cpp
reflexes. Press its button and it winks before you let go — that reaction is
a Rete rule firing in microseconds, zero tokens. Hold the button and it
thinks — that one is an LLM agent choosing an expression, a chirp, and a
mood color.

Status: scaffold. Firmware and brain land next; the design spec lives in
[tiny_agent](https://github.com/tinyagent-cc/tiny_agent)
`docs/superpowers/specs/2026-08-22-pip-companion-design.md`.

## Hardware

| Part | Role |
|---|---|
| Raspberry Pi Pico 2 W | body: face, senses, sound |
| ILI9341 240x320 SPI | the face |
| VEML7700 | ambient light |
| MAX98357A I2S amp + speaker | chirps |
| Tactile button, RGB LED | interaction, mood |
| Raspberry Pi Zero 2 W | brain: tiny_agent + rete_cpp |
| Raspberry Pi 5 | llama-server |

`pip-lite`: the same firmware built for a bare Pico 2 W (internal temp
sensor + onboard LED), so the demo runs with no extra parts.

## Layout

    firmware/   Pico SDK C++ (body)
    brain/      tiny_agent + rete_cpp process (judgment + reflexes)

Wiring, build, and flash instructions arrive with the firmware.
```

- [ ] **Step 2: PROTOCOL.md v0**

```markdown
# Pip body protocol v0

Plain HTTP JSON on the LAN. The body (Pico) serves; the brain calls.
No TLS: this is a same-LAN demo protocol, documented as such.

## Body endpoints

POST /express  {"emotion": "idle|happy|sleepy|thinking|alert|wink"} -> {"ok": true}
POST /chirp    {"name": "rise|trill|drop|purr"}                     -> {"ok": true}
POST /led      {"r": 0-255, "g": 0-255, "b": 0-255}                 -> {"ok": true}
GET  /senses   -> {"light_lux": float, "temp_c": float, "button": "up|down"}

Unknown emotion/chirp names: 400 with {"error": "..."}.

## Events (body -> brain webhook)

POST <brain_url>/event with one of:
  {"event": "button.press"}
  {"event": "button.hold"}      (fires once at 1.5s held)
  {"event": "button.release"}
  {"event": "light.low"}        (lux under threshold for 30s)
  {"event": "light.high"}       (lux back over threshold)

Delivery is at-most-once; the body does not retry. The brain treats events
as facts, not commands.

## Versioning

Breaking changes bump the version in this file and in the firmware's
GET /senses response header `X-Pip-Protocol: 0`.
```

- [ ] **Step 3: LICENSE and .gitignore**

MIT license, copyright `2026 Riadh Haj Amor` (copy the header/body shape from `~/git/tiny_agent_cpp/LICENSE`, it is MIT). `.gitignore`:

```
build/
config.h
*.uf2
.DS_Store
```

- [ ] **Step 4: Create repo, push, verify**

```bash
cd ~/git/pip && git init -b main && git add -A
git commit -m "docs: Pip scaffold — story, hardware, body protocol v0

<trailer>"
gh repo create tinyagent-cc/pip --public --source . --push \
  --description "A desk companion with a face — Pico 2 W body, tiny_agent brain, rete_cpp reflexes" \
  --homepage "https://tinyagent.cc"
gh repo view tinyagent-cc/pip --json description -q .description
```

Expected: the description echoes back.

- [ ] **Step 5: Report the two UI-only items for Riadh**

Pin `tiny_agent`, `rete_cpp`, `pip` on the org page; set the org display name to `tinyagent`. (API cannot pin org repos.)

## Self-review notes

Spec coverage: Track 4 fully (profile, descriptions, cross-links, pin list); Track 1's "repo scaffold" staging item. Types: n/a (no code). No placeholders: README/PROTOCOL content is written verbatim above; the one conditional (logo path) carries its own verification command.
