# Pip "wow demo" design: face v2, voice, wire link, eyes and ears

Date: 2026-08-23. Supersedes the Pip v1 scope in `2026-08-22-pip-companion-design.md`
(Track 1) where they differ; everything not mentioned here stays as built
(pip main 16558ce: body firmware v0, brain on the Pi Zero, PROTOCOL v0).

## Why

Pip works end to end but the demo is mute and the screen shows only two
eyes, so a press looks like nothing happened and nobody can see the
reflex-vs-judgment story. This spec turns Pip into a filmable desk companion:
a face with a mouth and moods, a voice, a status strip that explains what
each part is doing, a wire to its brain, and eyes and ears on the Jetson.
Every scene captions itself, so the film needs no voice-over.

## The fleet and each part's role

| Device | Role | Link |
|---|---|---|
| Pico 2 W | **body**: face, chirps, speech playback, light/temp/button senses | UART to the Zero; WiFi HTTP for config/debug and as fallback |
| Pi Zero 2 W | **brain**: reflex rules (rete) in microseconds, judgment agent, scene director, HUD stats | UART to the Pico; LAN to Pi 5 and Jetson |
| Jetson Orin Nano | **cortex**: LLM + VLM on the GPU (Qwen2.5-VL-3B via llama-server), Whisper STT, Brio 4K camera + mic | LAN |
| Pi 5 | **fallback mind + voice**: llama-server Qwen2.5-3B (`--llm2`), Piper TTS service | LAN |

The story, in one line per part: reflexes are rules (Zero, µs); judgment is
an agent (Jetson, seconds); when the cortex is gone the agent falls back
(Pi 5); the body only ever shows, sounds and senses.

## Wiring (adds to `hardware/pins.hpp` and the pictorial)

Pico UART0 to the Zero's PL011, 3.3 V both sides, no level shifter:

| Pico | Pico header pin | Zero header pin | Zero GPIO |
|---|---|---|---|
| GP0 (UART0 TX) | 1 | 10 | GPIO15 RXD |
| GP1 (UART0 RX) | 2 | 8 | GPIO14 TXD |
| GND | 3 | 6 | GND |

Zero one-time setup (sudo, Riadh runs it): in `/boot/firmware/config.txt`
`enable_uart=1` and `dtoverlay=disable-bt`; remove `console=serial0,115200`
from `/boot/firmware/cmdline.txt`; `sudo systemctl disable hciuart`; reboot.
The brain opens `/dev/ttyAMA0` at 921600 8N1. Bluetooth on the Zero is not
used by Pip. The Pico stays on the Mac's USB for flashing and serial logs.

Brio 4K: USB on the Jetson; UVC camera `/dev/video0` and its mic as an ALSA
capture device. Pip has no microphone of its own.

## Body firmware v1 (`firmware/`, `core/`)

### Screen
- Layout (320x240 landscape, unchanged MADCTL): face region 320x200 on top,
  HUD strip 320x40 at the bottom.
- **Face v2**: eyes (ellipse + pupil, as now) plus brows (two short thick
  lines, angle per emotion) and a mouth (a line or arc: flat, smile, frown,
  "o"). Emotions: `idle, happy, sleepy, thinking, alert, wink, surprised,
  sad, listening, talking`. `listening` shows a small animated ear/waveform
  glyph beside the face; `talking` animates the mouth open/closed while
  speech plays and returns to the previous emotion when audio ends.
- **Idle life**: blink every 3-6 s (random), saccade (pupils shift) every
  2-5 s, both only in `idle`/`happy`/`sleepy` (slow in sleepy).
- **Return to idle**: `wink`, `alert`, `surprised` hold 3 s then return to
  idle; `sleepy` holds while `night` is set; `thinking`/`listening` hold
  until the brain changes them; `happy`/`sad` hold 10 s.
- **Mood tint**: background colour per emotion (idle dark blue-grey, happy
  warm, sleepy near-black, alert amber, sad grey-blue, thinking violet).
- **Speech bubble**: `POST /say {"text"}` shows up to 2 lines of text in a
  rounded box above the mouth for max(3 s, audio length) with the 5x7 font
  scaled x2 (10x14 px), word-wrapped; the same text is what gets spoken if
  audio follows.
- **HUD strip** (5x7 font, 1x): left, lux bar (0-200 lux log scale) with a
  moon glyph when night; temp in C; centre, the headline
  `reflex 95us  judge 5.8s` (values pushed by the brain); right, link state
  glyphs: wire (UART frames seen in the last 2 s), wifi, brain (brain ok),
  cortex (Jetson ok), and which mind answered last (`J` or `5`).
  Pushed by `POST /hud {"reflex_us","judge_ms","brain","cortex","mind","scene"}`;
  anything omitted keeps its last value; `scene` shows as a one-word caption
  on the HUD's top line.
- **Font**: a 5x7 ASCII bitmap (95 glyphs, 665 bytes) in `core/`, with a
  `draw_text(x,y,scale,colour)` primitive, plus `draw_line`, `draw_arc`
  (thick), `draw_round_rect`. All rendering stays dirty-rect based; the HUD
  redraws only on value change.
- Frame budget stays 30 fps; the framebuffer stays one 150 KB RGB565 buffer.

### Sound
- **Chirp synth** (finally real): `pico_audio_i2s` at 16 kHz mono s16,
  DMA, a 64 KB ring. Chirps are baked envelopes: `rise`, `trill`, `drop`,
  `purr`, `boot`, `sad`, each 150-600 ms, generated at boot into flash-side
  tables or synthesized on the fly (sine + ADSR), no audio files.
- **Speech playback**: PCM frames arrive over the link (below) and are
  written into the same ring; chirps pre-empt speech (mix: chirp replaces
  for its duration). `talking` face while the ring drains; `senses`
  reports `"audio":{"free":bytes,"playing":bool}`.
- WiFi-only mode (no wire): chirps work, speech does not (HTTP bodies stay
  1 KB; no audio over HTTP).

### The link (`core/` codec, platform-free; `firmware/src/net/link.cpp`)
Frames both ways on UART0 at 921600:
`0xA5 | type u8 | len u16 LE | payload | crc8(type,len,payload)`.
- type `0x01` JSON: UTF-8, the same objects as HTTP: up (`{"event":"button.press"}`,
  `{"senses":{...}}` every 500 ms, `{"hello":{"fw":"...","protocol":1}}` at boot),
  down (`{"cmd":"express","emotion":"wink"}`, `{"cmd":"chirp","name":"rise"}`,
  `{"cmd":"led","r":..}`, `{"cmd":"say","text":"..."}`, `{"cmd":"hud",...}`,
  `{"cmd":"scene","name":"..."}`, `{"cmd":"ping"}` → `{"pong":true}`).
- type `0x02` AUDIO: raw s16 mono 16 kHz, ≤512 B payload; the sender paces
  at real time; the Pico drops a frame and counts it if the ring is full.
- Resync on any CRC or sync error by scanning for the next `0xA5`; counters
  `link.rx_frames, link.rx_bad, link.audio_dropped` in `/senses`.
- Events go to the wire when it is alive (a frame seen in the last 2 s),
  otherwise to the HTTP brain URL as today. Commands are accepted from both.
- The HTTP server keeps v0 routes and adds `/say`, `/hud`, `/scene`;
  `X-Pip-Protocol: 1`.

### Button
Unchanged FSM (press/hold at 1.5 s/release). Semantics move to the brain:
hold now means "talk to me".

## Brain v1 (`brain/`)
- `IBody` gains `say(text)`, `hud(json)`, `scene(name)`, `speak(pcm)`.
  New `LinkBody` (UART, the default when `--link /dev/ttyAMA0` is given) is
  both an `IBody` and the event source; `HttpBody` stays for `--pip URL`.
  The event queue/worker/reflex engine are unchanged.
- Reflex rules unchanged, plus `hold-listen` replaces `hold-think`:
  `express(listening) + chirp(rise)`; and `say` as a new tool for the agent.
- **Judgment flow on `button.hold`**: 1) ask the cortex to listen
  (`POST http://orin:8090/listen {"seconds":4}` → `{"text"}`), HUD `listening`;
  2) `express(thinking)`; 3) run the agent with the transcript (or, if the
  cortex is down or the transcript is empty, the old "someone held your
  button" prompt) and tools `express, chirp, led, say, look`; `look` calls
  `POST http://orin:8090/see {"question"}` and returns the VLM's sentence;
  4) the agent's final sentence and every `say` go to TTS
  (`POST http://pi5:8091/tts {"text"}` → s16 16 kHz) and stream to the body
  while the bubble shows the text; 5) HUD gets `judge_ms` and `mind` (`J`
  Jetson, `5` Pi 5, via which model answered: `model_fallback` is observed by
  the usage middleware, which records the base_url of the model that replied).
- **Director**: `POST /scene {"name"}` on the brain and `--tour` flag. Scenes
  are small scripts in `brain/src/scenes.cpp` (say + hud caption + express +
  led, with waits), each also reachable by name from the README:
  1. `reflex` : "Press me." → wink in µs → caption `reflex 95us`; "Now hold
     me and ask something." → the judgment flow → caption `judge 5.8s`.
  2. `night`  : "Cover my light sensor." → sleepy, LED dim; the agent is
     told to set a bright LED, guardrail caps it, caption `rule capped 255->40`.
  3. `fallback`: caption "cortex offline"; next hold is answered by the Pi 5,
     caption `mind: Pi 5 (fallback)`. (Riadh powers the Jetson server down,
     or the scene asks the Jetson service to pause itself for 60 s.)
  4. `fever`  : "Warm my chip." → hot-alert, red LED, caption.
  5. `who`    : "Who's there?" → `look` → Pip says what the camera sees.
  6. `tour`   : 2-minute unattended loop: Pip introduces each part in one
     sentence each (body, brain, cortex, fallback, voice), runs `reflex`
     (simulated press via the link: the brain injects `button.press`),
     `night` (simulated light.low/high), and `who`, then returns to idle.
- `/health` gains `link`, `cortex`, `voice` booleans and `mind`.
- Flags: `--link /dev/ttyAMA0 --baud 921600 --cortex http://orin-desktop.local:8090
  --voice http://pi5.local:8091 --llm http://orin-desktop.local:8081
  --llm2 http://pi5.local:8081 --tour`.

## Services
- `services/cortex/` (Jetson, Python 3, FastAPI already installed,
  systemd user unit): `POST /listen {"seconds"}` records from the Brio mic
  (`arecord`, 16 kHz mono) and transcribes with whisper.cpp CUDA build
  (`base.en`), returns `{"text","ms"}`; `POST /see {"question"}` grabs a
  frame from `/dev/video0` (OpenCV or `v4l2-ctl`), sends it with the
  question to the local llama-server multimodal chat (`/v1/chat/completions`
  with an `image_url` data URI), returns `{"text","ms"}`; `GET /health`.
  Probed 2026-08-23: Qwen2.5-VL-3B on this llama.cpp build returns no
  `tool_calls`, breaks image answers at 4k context, and fills the 8 GB. So
  the Jetson runs two servers: `Qwen2.5-3B-Instruct` Q4 (text, tool calls,
  GPU) on :8081 for judgment, and a small VLM (SmolVLM-500M-Instruct class
  GGUF + mmproj, or Qwen2.5-VL-3B with `--image-min-tokens 1024 -c 8192` only
  if the small one is not good enough) on :8082 for `/see`. whisper.cpp
  (CUDA, `base.en`) for `/listen`. Plan C's first task measures the trio's
  resident memory and latency; budget: under 6.5 GB with no swap in use.
- `services/voice/` (Pi 5, Python 3, systemd user unit): Piper TTS,
  `en_US-lessac-medium` (or another small voice, chosen by ear on the bench),
  resampled to 16 kHz s16 mono, pitch +2 semitones via `sox` or a Piper
  `length_scale`/`noise` tweak to sound like a pet; `POST /tts {"text"}` →
  raw PCM body, `GET /health`. Voice choice is a bench decision, not a spec one.
- Both services: no auth, same LAN, documented like the brain.

## Interfaces summary (the contracts plans are written against)
- Link frame and JSON objects as above; PROTOCOL.md v1 documents the link,
  the new routes and the HUD/say payloads.
- `IBody::say/hud/scene/speak`; `LinkBody` event source; cortex and voice
  HTTP shapes above.

## Error handling
- Wire dead: body falls back to HTTP events; brain marks `link=false` on HUD;
  everything but speech keeps working.
- Cortex down: `/listen` or `/see` fails → judgment runs the old prompt
  without transcript; `look` tool returns "I can't see right now"; HUD `cortex` off.
- Voice down: text bubble only; a `drop` chirp marks the missing voice.
- Jetson LLM down: `model_fallback` to the Pi 5; HUD `mind: 5`.
- Audio ring full: frames dropped and counted, never blocks the face loop.
- All service calls have 1-5 s connect timeouts; the worker never blocks on
  the voice stream (speech streaming runs on its own thread with a bounded
  queue).

## Testing
- `core/` host tests: font/text rendering into a framebuffer with golden
  pixel checks for a few glyphs; face v2 frames per emotion render without
  out-of-bounds writes; link codec round-trip, resync on garbage, CRC;
  return-to-idle timers table-driven.
- Firmware: builds for `pip`, `pip-lite` (no screen/audio), device configs
  warning-free; a `link-echo` smoke mode.
- Brain: `FakeLink` (in-memory pipe) driving events and capturing commands
  and audio frames; scenes run against FakeBody with a fake clock; judgment
  flow with FakeCortex/FakeVoice HTTP fakes; fallback path forced by a dead
  `--llm` and a live FakeLlm as `--llm2`, asserting `mind`.
- Services: unit tests with recorded fixtures (a WAV, a JPEG) and a mocked
  llama-server; `curl` smoke in the README.
- Bench acceptance (Riadh present): every scene filmed once; `tour` runs
  three times unattended.

## Staging (each its own SDD plan, each ends on the bench)
A. Firmware screen v2 + font + HUD + bubble + idle life + return-to-idle +
   the link codec and UART transport + v1 HTTP routes + pictorial/pins update.
B. Firmware sound: chirp synth + PCM ring over the link + `talking`.
C. Cortex and voice services + brain v1 (LinkBody, say/hud/scene, judgment
   flow with listen/look, director and tour, fallback HUD, deploy scripts
   for Zero/Jetson/Pi 5).
D. Film script and README "anatomy of Pip", landing-page section, the
   bench acceptance run.

## Success criteria
- Press to wink under 20 ms measured at the brain over the wire.
- Hold to spoken answer under 10 s with the Jetson, under 40 s on fallback.
- `who` scene names something real in the room; `night` shows the cap;
  `fallback` survives killing the Jetson server; HUD numbers visible on film.
- `tour` runs three times unattended; README explains each part's role with
  one screenshot per scene.

## Out of scope
Touch input, battery, wake word (push-to-talk only), face tracking,
multi-language voice, speech over WiFi, any cloud model.
