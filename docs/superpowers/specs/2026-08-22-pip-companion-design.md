# Pip: the tinyagent desk companion — design

Date: 2026-08-22. Status: approved direction, pre-implementation.
Owner: Riadh. Repos: `tinyagent-cc/pip` (new), `tinyagent-cc/tiny_agent`,
`tinyagent-cc/rete_cpp`, `tinyagent-cc/.github` (new).

## What this is

A staged set of demos that pushes tiny_agent's edge story down to a
microcontroller, wires rete_cpp into working code, and produces filmable
material for tinyagent.cc and the incubator dossier.

Four tracks, in priority order:

1. **Pip**, a desk companion with a face: Pico 2 W body, tiny_agent brain.
2. **Model bake-off**: which small models can actually drive an agent on
   Pi-class hardware.
3. **On-Pico spike**: can tiny_agent's core loop run bare-metal on RP2350.
   Answer-only; code is throwaway unless it's a go.
4. **Org face-lift**: tinyagent-cc looks like a project, not an empty shell.

## Track 1: Pip

### The story

Pip sits on the desk. It blinks, looks around, chirps. Press its button and
it winks instantly; hold the button and it thinks, then reacts with an
expression, a chirp, and a mood color. Turn the lights off and it gets
sleepy. The instant reactions are Rete rules firing in microseconds on a
512MB computer; the considered reactions are an LLM agent reasoning over
tools. Reflexes are rules; judgment is an agent.

### Hardware (all owned, verified from photos 2026-08-22)

| Part | Role | Bus |
|---|---|---|
| Pico 2 W (RP2350) | body: face, senses, sound | WiFi to brain |
| ILI9341 240x320 SPI + touch | the face | SPI0 @ 40MHz |
| VEML7700 | ambient light sense | I2C0 |
| MAX98357A I2S amp + speaker | voice (chirps) | I2S via PIO |
| Tactile push button | interaction | GPIO, internal pull-up |
| RGB LED (4-pin, diffused) | mood light | 3x PWM + resistors |
| Pi Zero 2 W | brain: tiny_agent + rete_cpp | WiFi |
| Pi 5 | LLM: llama-server | LAN |

Onboard fallback tier: a `pip-lite` build using only the Pico 2 W (internal
temp sensor + onboard LED) so anyone with the $7 board and no parts can run
the demo. Both builds live in the same firmware, selected by a CMake option.

Explicitly out of scope for v1: the touch panel (face is output-only), the
MPI3508, TTS/speech (chirps only), NS4168 (kept spare), battery power.

### Body: Pico firmware (`tinyagent-cc/pip`, C++ on Pico SDK)

- **Face engine**: eyes-first design (two large eyes, lids, pupils), ~6
  expressions: idle/blink, happy, sleepy, thinking, alert, wink. Region-based
  redraw, double-buffered rectangles, no full-frame pushes. 30fps idle
  animation budget.
- **Chirp engine**: I2S out via PIO (pico-extras audio_i2s), small baked
  waveform/envelope synth (rise, trill, drop, purr). No audio files.
- **Tool server**: lwIP HTTP/JSON on the Pico:
  `POST /express {"emotion":"happy"}`, `POST /chirp {"name":"trill"}`,
  `POST /led {"r":..,"g":..,"b":..}`, `GET /senses` (light lux, temp, button
  state). Flat JSON, no TLS (LAN demo; documented).
- **Event push**: on button press/hold/release and light-level bands crossing
  a threshold, the Pico POSTs `{"event":...}` to the brain's webhook URL
  (set at boot via config). No polling.
- WiFi credentials + brain URL in a `config.h` generated from
  `config.example.h`; never committed.

### Brain: tiny_agent + rete_cpp on the Pi Zero 2 W

One C++ process, in `pip/brain/`:

- **Reflex layer (rete_cpp)**: events become facts; rules fire body calls
  directly. Examples: `button.press -> wink + chirp(rise)` (target: reaction
  under 50ms end-to-end on LAN), `light.low sustained -> express(sleepy)`,
  `light.high after low -> express(alert) + chirp(trill)`,
  `temp > threshold -> express(alert) + led(red)`. Refraction stops
  re-firing; the agenda's conflict resolution is the demo of rete_cpp doing
  real work.
- **Judgment layer (tiny_agent)**: fires only on `button.hold` ("talk to
  me") and on events no rule matched. The agent gets Pip's tools (express,
  chirp, led, senses) plus recent event history in context, and decides the
  reaction. Model access via the OpenAI-compatible provider pointed at the
  Pi 5's llama-server; `model_fallback` middleware chains small-local to
  bigger-remote.
- Every LLM decision is logged with tokens and latency; every reflex with
  its rule name and firing time. The contrast (microseconds vs seconds,
  zero tokens vs N) is the headline metric and goes on the landing page.

### Interfaces

Body and brain share one JSON contract, versioned in `pip/PROTOCOL.md`.
The brain treats the body as a tiny_agent toolset; nothing in tiny_agent
core changes for Pip. If the on-Pico spike (track 3) is a go, the same
protocol lets the loop migrate onto the Pico without redesign.

### Testing

- Firmware: expression/chirp table-driven unit tests compiled host-side
  (face engine renders to a memory framebuffer, golden-image compare);
  hardware smoke checklist in the README.
- Brain: rete rules under Catch2 (fact in, expected activations out);
  agent loop against a mock llama-server; one end-to-end script that fakes
  Pico events over HTTP and asserts body calls.
- Demo acceptance: 2-minute filmable loop (idle, press, hold+think, lights
  off, lights on) runs three times unattended.

## Track 2: model bake-off

Question: what is the smallest model that can reliably drive Pip, and what
does the capability ladder on Pi-class hardware look like in late 2026?

- Candidates (hypotheses, re-verified against current releases at build
  time per the delegation doctrine): Gemma 4 E2B and E4B, Qwen3 0.6B and
  1.7B, SmolLM3, LFM2 1.2B, plus one ~3B-class ceiling model. GGUF via
  llama.cpp on the Pi 5; the sub-1B tier also measured on the Zero 2 W
  itself (can the brain and the LLM share the 512MB board?).
- Harness: a tiny_agent example (`examples/18_model_bakeoff` or next free
  number) driving each model through the real agent loop against a fixed
  20-case tool-calling suite drawn from Pip's contract: valid JSON rate,
  correct tool choice, correct argument extraction, refusal on nonsense.
  Plus tok/s and peak RSS per model.
- Output: results table in `docs/benchmarks.md`, a section on tinyagent.cc,
  and a concrete routing recommendation for Pip's `model_fallback` chain.
  Method and one-command re-run documented so numbers age gracefully.

## Track 3: on-Pico spike

Feasibility only: compile tiny_agent's core loop for RP2350 (arm-none-eabi,
picolibc) with lwIP sockets and plain-HTTP chat completions against
llama-server. Known risks to probe in order: C++20 stdlib coverage,
exceptions on bare metal, heap pressure in 520KB, blocking sockets in the
agent loop. Output: a written go/no-go with the first blocking wall named,
or a minimal llama-server round-trip from the Pico. Timebox before code:
one day equivalent. Anything built is labeled throwaway.

## Track 4: org face-lift (bounded)

- `tinyagent-cc/.github` with `profile/README.md`: the mark, one-line
  positioning, the layer story (tiny_agent runs the agent, rete_cpp is the
  reflex layer, Pip is the proof), links to tinyagent.cc and the demos.
- Org description + website URL set via API.
- rete_cpp repo description set ("Header-only C++17 Rete rules engine —
  the reflex layer of the tinyagent stack") and a positioning paragraph +
  cross-links added to both repo READMEs.
- Riadh, UI-only: pin tiny_agent + rete_cpp (+ pip once real), org display
  name.

## Staging

1. Org face-lift (hours) and Pip repo scaffold.
2. Pip firmware: face engine first (filmable on day one), then tools/events,
   then audio.
3. Brain: reflex layer, then agent layer, then end-to-end demo film.
4. Bake-off harness and first results (needs only Pi 5 + llama.cpp; can
   start parallel to firmware).
5. Spike report.
6. Landing page: Pip section + bake-off table + film.

## Success criteria

- Pip runs the 2-minute demo loop unattended, three consecutive times.
- Reflex reactions under 50ms; the reflex-vs-judgment metric logged and
  shown.
- Bake-off table published with a reproducible harness; a model routing
  choice made from data, not vibes.
- Spike question answered in writing either way.
- tinyagent-cc org page tells the story to a stranger in one screen.
