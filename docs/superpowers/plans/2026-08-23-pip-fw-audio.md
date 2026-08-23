# Pip firmware v1, Plan B: sound (chirp synth, PCM speech ring over the link, talking face)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pip makes sound: six synthesized chirps, and speech streamed from the brain as 16 kHz PCM over the UART link, played through the MAX98357A, with the face animating `talking` while speech plays.

**Architecture:** `core/` gets a chirp synthesizer (pure functions, host-tested) and a lock-free single-producer/single-consumer PCM ring with chirp pre-emption. `firmware/` adds `audio.cpp` on `pico_audio_i2s` (pico-extras) at 16 kHz, fed from the main loop each frame; link AUDIO frames write into the ring; `/senses` reports `audio.free` and `audio.playing`; the face gets `set_talking` from the ring state. `pip-lite` keeps audio out.

**Tech Stack:** C++17, pico-extras `pico_audio_i2s` (DMA, PIO), pins from `firmware/pins.hpp` (`I2S_BCLK 26, I2S_LRCLK 27, I2S_DIN 28`). Bench-proven on 2026-08-23 by `firmware/smoke/bench.cpp` (440 Hz bursts, 44.1 kHz stereo S16).

**Spec:** `docs/superpowers/specs/2026-08-23-pip-wow-demo-design.md` (in `~/git/tiny_agent_cpp`), "Body firmware v1 → Sound" and "The link" (AUDIO frames). **Depends on Plan A** (`2026-08-23-pip-fw-v1-screen-link.md`) being merged: `Face::set_talking`, `AudioStats` in `Senses`, `link_poll(body, on_audio)`.

## Global Constraints

- Audio format everywhere: s16 mono 16 kHz. The I2S output is stereo S16 at 16 kHz (both channels carry the same sample), because `pico_audio_i2s` wants stereo pairs; document that in `audio.hpp`.
- Ring: 32768 samples (64 KB) static in `.bss`, SPSC: producer = main loop (link frames, chirps), consumer = the audio fill in the main loop too (single-threaded: no IRQ consumer; `pico_audio_i2s` buffers are filled with `take_audio_buffer(pool, false)` non-blocking each frame). Pool: 4 buffers × 512 stereo frames (128 ms of audio buffered ahead).
- Chirps pre-empt speech: while a chirp plays, its samples replace speech samples (speech samples are still consumed, so timing stays aligned).
- Never block the frame loop: the fill takes at most the free buffers; if the ring is empty, output silence.
- Chirp names v1: `rise, trill, drop, purr, boot, sad` (enum order `Rise, Trill, Drop, Purr, Boot, Sad`); each 150-600 ms, peak ≤ 20000.
- `pico_extras_import.cmake` is included for the main `pip` target (not only the smoke) unless `PIP_LITE`; a missing `PICO_EXTRAS_PATH` fails configure with a clear message.
- Builds warning-free: `pip`, `pip` lite, host tests green; commit per task; README/PROTOCOL truthful in the same commit.
- Build lines: as Plan A (host: `cmake -S tests -B build-tests -G Ninja && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure`; firmware: `export PATH=$HOME/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin:$PATH PICO_SDK_PATH=$HOME/git/pico-sdk PICO_EXTRAS_PATH=$HOME/git/pico-extras; cmake -S firmware -B build-fw -G Ninja -DPICO_EXTRAS_PATH=$HOME/git/pico-extras && cmake --build build-fw`).

---

### Task 1: Chirp synthesizer (core)

**Files:** create `core/include/pip/chirps.hpp`, `core/src/chirps.cpp`; modify `core/include/pip/protocol.hpp`, `core/src/protocol.cpp` (enum + names); test `tests/test_chirps.cpp`; `tests/test_protocol.cpp` (names).

**Interfaces:**
```cpp
namespace pip {
constexpr int AUDIO_RATE = 16000;
constexpr size_t CHIRP_MAX_SAMPLES = 9600;                       // 600 ms
// Renders chirp c into out (mono s16 16 kHz). Returns the sample count (<= CHIRP_MAX_SAMPLES, <= cap). Deterministic.
size_t render_chirp(Chirp c, int16_t* out, size_t cap);
}
```
Recipes (sine with a linear-ramp ADSR of 10 ms attack / 30 ms release, amplitude 12000 unless said):
- `rise`: sweep 600→1200 Hz over 200 ms.
- `trill`: 5 segments of 60 ms alternating 1000/1300 Hz (300 ms).
- `drop`: sweep 1000→400 Hz over 250 ms.
- `purr`: 300 Hz carrier, amplitude-modulated by a 40 Hz sine (depth 80 %), 400 ms, amplitude 8000.
- `boot`: three notes 523, 659, 784 Hz, 120 ms each with 10 ms gaps (≈ 380 ms).
- `sad`: 500 Hz 200 ms then 350 Hz 250 ms (450 ms), amplitude 10000.
- [ ] Tests: each chirp returns a count within [2400, 9600] and ≤ cap; peak |sample| ≤ 20000 and ≥ 6000; first and last 5 samples are near zero (|s| < 600: the envelope); `render_chirp(Rise, out, 100)` returns 100 (cap respected); `chirp_from("boot")` and `chirp_name(Chirp::Sad)=="sad"`.
- [ ] Implement, pass, commit `feat(core): chirp synthesizer, six chirps at 16 kHz`.

---

### Task 2: PCM ring with chirp pre-emption (core)

**Files:** create `core/include/pip/audio_ring.hpp`, `core/src/audio_ring.cpp`; test `tests/test_audio_ring.cpp`.

**Interfaces:**
```cpp
namespace pip {
class AudioRing {   // SPSC, power-of-two capacity, single core use on the Pico (no atomics needed beyond volatile indices)
public:
    static constexpr size_t CAP = 32768;                        // samples
    size_t write(const int16_t* s, size_t n);                   // appends up to free(); returns written
    size_t free() const; size_t used() const;
    void clear();
    void preempt(Chirp c);                                      // renders the chirp into the side buffer and starts it now (replaces a chirp already playing)
    bool chirp_playing() const;
    bool speech_playing() const { return used() > 0; }
    bool playing() const { return chirp_playing() || speech_playing(); }
    // Produces n output samples: chirp samples while a chirp plays (consuming speech underneath), else speech, else 0. Returns n always.
    size_t pull(int16_t* out, size_t n);
private:
    int16_t buf_[CAP]; volatile size_t head_ = 0, tail_ = 0;
    int16_t chirp_[CHIRP_MAX_SAMPLES]; size_t chirp_len_ = 0, chirp_pos_ = 0;
};
}
```
- [ ] Tests: write 1000, used()==1000, pull 400 returns those samples in order, used()==600; write beyond free() truncates and returns the written count; `preempt(Rise)` then pull 100 → samples equal `render_chirp(Rise)` first 100 and the speech head advanced by 100; after the chirp ends pull returns speech again; pull on empty → zeros; clear() → used 0 and chirp stopped.
- [ ] Implement, pass, commit `feat(core): PCM audio ring with chirp pre-emption`.

---

### Task 3: Firmware audio output, link audio in, talking face, senses

**Files:** create `firmware/src/audio.hpp`, `firmware/src/audio.cpp`; modify `firmware/src/main.cpp`, `firmware/src/net/link.cpp` (audio callback), `firmware/src/body.hpp` (AudioStats publish), `firmware/CMakeLists.txt`, `README.md`, `PROTOCOL.md` (chirp names, audio stats).

**Interfaces:**
```cpp
namespace pip::audio {
bool init();                          // pico_audio_i2s 16 kHz stereo S16, pool 4x512; false if setup failed (log, continue without sound)
void pump();                          // fills every free buffer from the ring; call once per frame
AudioRing& ring();
AudioStats stats();                   // free bytes = ring.free()*2, playing = ring.playing()
}
```
- `main.cpp`: `audio::init()` after the LCD; `body.take_chirp(c)` → `audio::ring().preempt(c)` (the old stub print goes); `link_poll(body, [](const uint8_t* pcm, uint16_t len){ size_t n = len/2; if (ring.free() < n) drop++ else ring.write((const int16_t*)pcm, n); })` — the drop counter lives in `net::link` stats (`audio_dropped`); `face.set_talking(audio::ring().speech_playing() && !chirp_playing())` each frame (edge-triggered: only call on change); `body.publish(..., link_stats(), audio::stats())`; on boot play `boot` once.
- CMake: `include(pico_extras_import.cmake)` when not `PIP_LITE`; `target_link_libraries(pip pico_audio_i2s)`; compile definitions `PICO_AUDIO_I2S_DATA_PIN=28 PICO_AUDIO_I2S_CLOCK_PIN_BASE=26` with the same static_asserts against `pins.hpp` that the smoke uses (put them in audio.cpp); the `audio.cpp` of pico-extras keeps its `-Wno-missing-field-initializers` property.
- [ ] Build both variants warning-free. Bench (controller or you, as Plan A allowed): flash; on boot the `boot` chirp plays; `curl -X POST http://192.168.1.110/chirp -d '{"name":"trill"}'` plays; from the Zero, `brain/scripts/link-probe.py` (from the brain branch if merged, else a 30-line Python here under `firmware/smoke/link-audio.py`) streams a 2 s 440 Hz s16 16 kHz sine as AUDIO frames paced at real time → the speaker plays it and the face talks; `/senses` shows `audio.playing:true` during it and `link.audio_dropped` stays 0.
- [ ] README: sound section (format, ring, pre-emption, what plays over what link); PROTOCOL: chirp names v1 and the audio stats.
- [ ] Commit `feat(firmware): I2S audio, chirps, speech over the link, talking face`.

---

## Self-review
- Spec: chirp synth (six, envelopes, no files) ✔ T1; 64 KB ring, DMA via pico_audio_i2s, chirps pre-empt speech ✔ T2/T3; talking face while ring drains ✔ T3; `senses.audio` ✔ T3; WiFi-only mode (chirps yes, speech no) holds because speech only arrives on the link ✔; drop-and-count on full ring ✔.
