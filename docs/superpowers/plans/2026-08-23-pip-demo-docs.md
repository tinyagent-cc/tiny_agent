# Pip wow-demo, Plan D: film script, "anatomy of Pip" README, landing page, bench acceptance

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Someone who finds Pip (repo, tinyagent.cc, or the film) understands in two minutes what each part does and can reproduce every scene. Riadh walks in, reads one page, and films.

**Architecture:** Docs only, no code behaviour changes: `README.md` (pip repo) gets an "Anatomy of Pip" section with one rendered frame per scene; `docs/demo-script.md` is the shot list; `site/pip.html` on tinyagent.cc (the `site/` directory of `~/git/tiny_agent_cpp`) is a single HTML page linked from `site/index.html`; the org profile README's "pip (in progress)" section points at it. Frames come from the host renderer (`tests/render_frames` from Plan A) so they are exact pixels of what the screen shows, no photos needed.

**Spec:** `docs/superpowers/specs/2026-08-23-pip-wow-demo-design.md`, "Director" scenes, "Success criteria", and the fleet table. **Depends on Plans A, B, C1, C2 merged and deployed.**

## Global Constraints

- Prose a human reads follows the `humanize` and `posture` skills (read `~/.agents/skills/humanize/SKILL.md` and `~/git/mystaff/.claude/skills/posture/SKILL.md` first; no em dashes, no arrows in prose, no hedging filler, specific numbers from the bench).
- Every number (latencies, memory, sizes) is copied from a measurement written in a README or a report of this day; nothing invented.
- Images: PNGs under `docs/frames/` in the pip repo (≤ 60 KB each, 320x240 or 2x). No photos with real account data (none exist here, but keep the rule).
- Landing page: the `site/` folder is static (HTML+CSS+JS, no build step, Cloudflare Pages from `main` via the CNAME); `pip.html` reuses `style.css`, dark/light aware like `index.html`, under 40 KB, self-contained, no external scripts.
- Commit per task; push only the pip repo and tiny_agent_cpp (the controller decides when).

---

### Task 1: Frames per scene

- [ ] Extend `tests/render_frames.cpp` (Plan A) with a `--scene <name>` mode that renders the representative frame of each scene: `reflex` (wink + HUD `reflex 95us`), `judge` (thinking + bubble "Let me think..." + HUD `judge 5.8s J`), `night` (sleepy, moon glyph, caption `night`, LED cap line in the bubble "rule capped 255 to 40"), `fallback` (happy + HUD `mind 5`, caption `fallback`), `fever` (alert, caption `fever`, temp 36C), `who` (listening glyph + bubble "I see a desk and a keyboard"), `tour` (talking + bubble "I'm Pip."). Write PNGs to `docs/frames/<scene>.png` (2x nearest-neighbour upscale, 640x480).
- [ ] Commit `docs(frames): one rendered screen per scene`.

### Task 2: README "Anatomy of Pip" + demo script

- [ ] `README.md` (pip): new top section after the intro: the fleet table (device, role, what runs, link), a 6-line "how a press becomes a wink" and "how a hold becomes an answer" walk-through with the measured numbers, the scene gallery (frame + one paragraph + the curl that triggers it), the links to PROTOCOL.md, brain/README.md, services/README.md, hardware/.
- [ ] `docs/demo-script.md`: shot list for a 2-3 minute film: setup (what is on, how to check health in one command each), the six scenes in order with what to say, what Pip says back, what the HUD will show, expected timings, the fallback switch (`systemctl --user stop llama-text` on the Jetson, and `start` after), the `tour` command, and a troubleshooting table (wire dead, cortex down, voice down: what the HUD shows and the one command to check).
- [ ] Commit `docs: anatomy of Pip and the demo script`.

### Task 3: tinyagent.cc page

- [ ] `site/pip.html` in `~/git/tiny_agent_cpp`: hero (name, one line, the fleet in four cards), the scene gallery with the frames (copy the PNGs into `site/assets/pip/`), the reflex-vs-judgment numbers, links to the repo; add a "Pip" entry to `site/index.html` nav/projects; update `tinyagent-cc/.github` `profile/README.md` "pip" section (via `gh api` PUT or a clone) to link `https://tinyagent.cc/pip.html` and the repo, and drop "(in progress)" once the acceptance run passed.
- [ ] Commit `site: Pip page`.

### Task 4: Bench acceptance

- [ ] Run, with the real fleet: health of body (`/senses`), brain (`/health`: link, cortex, voice true), cortex, voice; each scene once via `POST /scene`; `tour` three times unattended (log the start/end times and any `note` lines from `/log`); press-to-wink latency from the brain log (`reflex` microseconds) and hold-to-answer seconds from `judge_ms`; record everything in `docs/acceptance-2026-08-23.md` with pass/fail per success criterion from the spec. Fix what fails if it is small; otherwise list it as open with the exact symptom.
- [ ] Commit `docs: acceptance run 2026-08-23`.
