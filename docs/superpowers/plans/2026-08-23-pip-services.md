# Pip services, Plan C2: cortex (Jetson) and voice (Pi 5)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two small HTTP services the brain calls: the cortex on the Jetson (ears: Brio mic + whisper.cpp; eyes: Brio camera + a VLM on llama-server; plus the text LLM the brain's judgment uses) and the voice on the Pi 5 (Piper TTS → 16 kHz s16 PCM), each as a systemd user unit with a deploy script and a curl smoke.

**Architecture:** Python 3 + FastAPI + uvicorn, stdlib subprocess for `arecord`, `whisper-cli`, `gst-launch-1.0`; httpx/urllib for the local llama-server. Config via environment variables with sane defaults for the real devices. Unit tests on the Mac mock every subprocess and HTTP call; the bench smoke runs on the devices.

**Tech Stack:** Python 3.10+ (Jetson: `/usr/bin/python3` with fastapi+uvicorn already in `~/.local`; Pi 5: `~/pip-probe/venv` with fastapi, uvicorn, piper-tts 1.7.0, voice `~/pip-probe/voices/en_US-lessac-medium.onnx` + `.json`), systemd user units, llama-server (`~/tools/llama.cpp/build/bin/llama-server`, CUDA) on the Jetson, whisper.cpp (`~/tools/whisper.cpp/build/bin/whisper-cli`, model `~/tools/whisper.cpp/models/ggml-base.en.bin`; the CUDA build is finishing in the background, log `~/pip-probe/whisper-build.log` ends with `WHISPER_BUILD_OK` when done).

**Spec:** `docs/superpowers/specs/2026-08-23-pip-wow-demo-design.md` (in `~/git/tiny_agent_cpp`), section "Services" and the cortex/voice shapes under "Brain v1".

**Repo:** `~/git/pip`, new directory `services/` only. Current main: `d9d6af2`.

## Global Constraints

- HTTP contracts (the brain is written against these, do not drift):
  - cortex `GET /health` → `{"ok":true,"whisper":bool,"vlm":bool,"camera":bool,"mic":bool}`; `POST /listen {"seconds":4}` → `{"text":"...","ms":N}` (empty text when nothing understood, never 500 for silence; 503 `{"error":...}` when the mic or whisper is missing); `POST /see {"question":"..."}` → `{"text":"...","ms":N}` (503 when the camera or VLM is down).
  - voice `GET /health` → `{"ok":true,"voice":"en_US-lessac-medium","sample_rate":16000}`; `POST /tts {"text":"..."}` → HTTP 200, `Content-Type: application/octet-stream`, body raw little-endian s16 mono 16 kHz PCM, header `X-Sample-Rate: 16000`, `X-Duration-Ms: N`; 400 on empty text; text capped at 300 chars.
- Ports: cortex :8090, voice :8091, Jetson text LLM :8081, Jetson VLM :8082. Never touch Pi 5 :8080 (Riadh's own llama-server); the Pi 5 :8081 Qwen2.5-3B server already runs there (leave it).
- Devices: Jetson `orin@orin-desktop.local` (key auth, no sudo), Pi 5 `pi@pi5.local` (key auth, no sudo), both have lingering user systemd. Never type passwords; never sudo.
- Jetson memory budget: text LLM + VLM + whisper + cortex < 6.5 GB resident, no swap in use, measured and written into `services/README.md`.
- Secrets: none in this plan. No credentials in source.
- Tests: `python3 -m pytest services/ -q` on the Mac (use a venv at `~/tools/pip-services-venv` with fastapi, httpx, pytest, numpy; create it if absent). Units never touch hardware.
- Commit per task.

---

## File structure

| File | Responsibility |
|---|---|
| `services/cortex/cortex.py` | FastAPI app: `/health`, `/listen`, `/see`; `Recorder` (arecord), `Transcriber` (whisper-cli), `Camera` (gst-launch frame grab), `Vlm` (llama-server chat with image) |
| `services/cortex/tests/test_cortex.py` | mocks subprocess + httpx; asserts shapes and error codes |
| `services/cortex/deploy/pip-cortex.service`, `llama-text.service`, `llama-vlm.service` | user units |
| `services/cortex/deploy/install-jetson.sh` | runs ON the Jetson: downloads the VLM GGUF+mmproj if missing (`hf download ggml-org/SmolVLM-500M-Instruct-GGUF` → `~/models/`), installs units, starts them, prints health |
| `services/scripts/deploy-jetson.sh` | runs on the Mac: rsync `services/cortex` to `orin@orin-desktop.local:~/pip-cortex/`, runs `install-jetson.sh` over ssh, curls health |
| `services/voice/voice.py` | FastAPI app: `/health`, `/tts`; Piper synth → resample 22050→16000 with a pitch/speed tweak (`PIP_VOICE_RATE=1.12` default: resampling ratio, raises pitch ~2 semitones and speed 12 %) |
| `services/voice/tests/test_voice.py` | mocks the Piper voice object; asserts PCM length, headers, 400 |
| `services/voice/deploy/pip-voice.service`, `install-pi5.sh`; `services/scripts/deploy-pi5.sh` | |
| `services/README.md` | what runs where, ports, env vars, curl smokes, measured memory/latency, how to choose another Piper voice |

---

### Task 1: Jetson baseline measurement and the two llama-servers

**Files:** `services/cortex/deploy/llama-text.service`, `llama-vlm.service`, `install-jetson.sh` (first half), `services/README.md` (measurements table).

- [ ] Check the whisper build finished (`ssh orin@orin-desktop.local 'tail -1 ~/pip-probe/whisper-build.log; ls ~/tools/whisper.cpp/build/bin/whisper-cli'`). If still building, continue with the llama tasks and come back.
- [ ] Units (user, `~/.config/systemd/user/`):
  - `llama-text.service`: `ExecStart=%h/tools/llama.cpp/build/bin/llama-server -m %h/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf --host 0.0.0.0 --port 8081 -ngl 99 -c 4096 --jinja -t 4`, `Restart=always`, `Environment=LD_LIBRARY_PATH=%h/tools/llama.cpp/build/bin`, `Environment=PATH=/usr/local/cuda/bin:/usr/bin:/bin`.
  - `llama-vlm.service`: same binary, `-m %h/models/SmolVLM-500M-Instruct-Q8_0.gguf --mmproj %h/models/mmproj-SmolVLM-500M-Instruct-f16.gguf --port 8082 -ngl 99 -c 2048`. (File names: check what `hf download ggml-org/SmolVLM-500M-Instruct-GGUF --local-dir ~/models/smolvlm` actually produces and point at those; `~/.local/bin/hf` exists on the Jetson.)
- [ ] Start both, wait for `/health` on each (`curl -s :8081/health`, `:8082/health`), then measure and record in README: `free -m`, per-process RSS (`ps -o rss,cmd -C llama-server`), `tegrastats --interval 1000 | head -2` if available, a text completion latency (`/v1/chat/completions` "Say hi in five words", measure ms) and a VLM latency with the probe frame `~/pip-probe/frame.jpg` as a data URI ("What is in this picture? One sentence."). Budget: total used < 6.5 GB, no swap. If the VLM answer is garbage or > 12 s, try `Qwen2.5-VL-3B` with `--image-min-tokens 1024 -c 8192` and record the comparison; pick the one that fits the budget and answers sensibly.
- [ ] Commit `feat(services): Jetson llama text + vlm units with measured footprint`.

---

### Task 2: cortex service

**Files:** `services/cortex/cortex.py`, `services/cortex/tests/test_cortex.py`, `services/cortex/deploy/pip-cortex.service`, `install-jetson.sh` (second half), `services/scripts/deploy-jetson.sh`.

**Env vars (defaults):** `PIP_ALSA_DEVICE=plughw:2,0` (the Brio; `arecord -l` to confirm the card index, write the real value into the unit), `PIP_WHISPER_BIN=~/tools/whisper.cpp/build/bin/whisper-cli`, `PIP_WHISPER_MODEL=~/tools/whisper.cpp/models/ggml-base.en.bin`, `PIP_CAMERA=/dev/video0`, `PIP_VLM_URL=http://127.0.0.1:8082`, `PIP_CORTEX_PORT=8090`, `PIP_FRAME_W=1280 PIP_FRAME_H=720`.

**Behaviour:**
- `/listen`: `arecord -D $DEV -f S16_LE -r 16000 -c 1 -d $seconds -q $tmp.wav` (seconds clamped 1..10), then `whisper-cli -m $MODEL -f $tmp.wav -nt -np -l en` and take stdout text (strip, collapse whitespace, drop `[BLANK_AUDIO]`/`(...)` bracket noise); returns `{"text","ms"}`. Missing binary/device → 503.
- `/see`: `gst-launch-1.0 -q v4l2src device=$CAM num-buffers=1 ! image/jpeg,width=$W,height=$H ! filesink location=$tmp.jpg` (10 s timeout; if the Brio needs a warm-up use `num-buffers=5` and keep the last by using `multifilesink`? Keep it simple: try `num-buffers=1`, and if the file is < 10 KB, retry once with `num-buffers=3 ! ... ! multifilesink location=$tmp-%d.jpg` and take the last). Then POST `/v1/chat/completions` to the VLM with `messages=[{"role":"user","content":[{"type":"text","text":question},{"type":"image_url","image_url":{"url":"data:image/jpeg;base64,..."}}]}]`, `max_tokens 80`, `temperature 0.2`; return the content. Camera/VLM failure → 503.
- `/health`: checks binaries exist, `$CAM` exists, `arecord -l` lists a card, VLM `/health` ok (1 s timeout).
- Concurrency: one `/listen` and one `/see` at a time (asyncio locks; a second caller waits).
- [ ] Tests (mock `subprocess.run` and `httpx.Client.post`): `/listen` happy path returns trimmed text and ms; whisper output with `[BLANK_AUDIO]` → `""`; arecord failing → 503; `/see` happy path builds the data URI and returns text; VLM 500 → 503; `/health` shape.
- [ ] `pip-cortex.service`: `ExecStart=/usr/bin/python3 -m uvicorn cortex:app --host 0.0.0.0 --port 8090`, `WorkingDirectory=%h/pip-cortex`, env vars, `Restart=always`, `After=llama-vlm.service`.
- [ ] `deploy-jetson.sh` (Mac): `rsync -a services/cortex/ orin@orin-desktop.local:~/pip-cortex/` then `ssh ... 'bash ~/pip-cortex/deploy/install-jetson.sh'` which installs all three units, `daemon-reload`, `enable --now`, waits for `:8090/health` and prints it. Then from the Mac: `curl -s -X POST http://orin-desktop.local:8090/listen -d '{"seconds":3}'` (say something near the Brio... the controller is alone, so accept `""` or room noise) and `curl -s -X POST .../see -d '{"question":"What do you see? One sentence."}'` → a sentence about the room. Record both outputs and latencies in README.
- [ ] Commit `feat(services): cortex service (listen via whisper, see via VLM) with Jetson deploy`.

---

### Task 3: voice service

**Files:** `services/voice/voice.py`, `services/voice/tests/test_voice.py`, `services/voice/deploy/pip-voice.service`, `install-pi5.sh`, `services/scripts/deploy-pi5.sh`.

**Env vars:** `PIP_PIPER_VOICE=~/pip-probe/voices/en_US-lessac-medium.onnx`, `PIP_VOICE_RATE=1.12`, `PIP_VOICE_PORT=8091`, `PIP_PIPER_LENGTH_SCALE=1.0`.

**Behaviour:** load the voice once at startup (`from piper import PiperVoice; PiperVoice.load(path)`); `/tts`: synthesize to int16 at the voice's native rate (piper-tts 1.7: `voice.synthesize(text, syn_config)` yields audio chunks; check the installed API on the Pi 5 with `python3 -c "import piper,inspect;print(inspect.signature(piper.PiperVoice.synthesize))"` before coding, and write the found signature into the module docstring), concatenate, then resample to 16000 Hz with numpy linear interpolation using an effective source rate of `native_rate / PIP_VOICE_RATE` (this raises pitch and speed by the rate factor: the "pet" tweak), clip to int16, return bytes; headers as in the constraints. Cap text at 300 chars (413 beyond that), 400 on empty.
- [ ] Tests (mock `PiperVoice.load` to return an object whose `synthesize` yields one chunk of 22050 samples of a sine): `/tts` returns `Content-Type: application/octet-stream`, `X-Sample-Rate: 16000`, body length == 2 × round(22050 × 16000/22050 / 1.12) ± 2 samples, `X-Duration-Ms` ≈ 893; empty text → 400; `/health` shape.
- [ ] `pip-voice.service`: `ExecStart=%h/pip-probe/venv/bin/python -m uvicorn voice:app --host 0.0.0.0 --port 8091`, `WorkingDirectory=%h/pip-voice`, `Restart=always`. `deploy-pi5.sh`: rsync to `pi@pi5.local:~/pip-voice/`, install unit, wait for health, then `curl -s -X POST http://pi5.local:8091/tts -d '{"text":"Hello, I am Pip."}' -o /tmp/pip.pcm && ls -la /tmp/pip.pcm` and play it on the Mac (`ffplay -f s16le -ar 16000 -ac 1 /tmp/pip.pcm` or `afplay` after wrapping into a WAV with Python) to confirm it sounds like speech; record latency.
- [ ] Commit `feat(services): voice service (Piper -> 16 kHz PCM) with Pi 5 deploy`.

---

### Task 4: README and smoke lines

**Files:** `services/README.md`.
- [ ] Table: device, service, port, unit name, how to restart, health curl, the measured numbers from Tasks 1-3, env vars, how to swap the Piper voice, the VLM choice and why. Commit `docs(services): README with ports, units, measurements`.

---

## Self-review
- Spec coverage: cortex listen/see/health ✔, two Jetson llama-servers + whisper ✔, memory/latency measurement ✔ (T1), voice tts/health with 16 kHz PCM and the pitch tweak ✔, units + deploy scripts ✔, unit tests with mocks + curl smoke ✔, README ✔.
- Contracts match Plan C1 (`Cortex`/`Voice` clients): paths, JSON keys, PCM format, status codes.
