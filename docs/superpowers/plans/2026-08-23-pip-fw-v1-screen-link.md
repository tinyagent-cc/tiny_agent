# Pip firmware v1, Plan A: screen v2, HUD, bubble, link codec + UART, v1 routes

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Pico body from "two eyes over HTTP" into a face with brows, mouth, moods, a speech bubble and a HUD strip, talking to its brain over a UART link (HTTP kept for config/debug/fallback).

**Architecture:** Everything renderable and parsable lives in `core/` (host-testable, no Pico SDK): a 5x7 font and draw primitives, Face v2 with idle life and return-to-idle timers, a `Hud` renderer, a `Bubble`, the link frame codec, and a shared command dispatcher used by both the HTTP routes and the link JSON. `firmware/` adds the UART transport (RX IRQ into a ring, main-loop decode) and routes events over the wire when it is alive. The main loop stays a single 30 fps loop; lwIP callbacks and the UART IRQ only fill mailboxes.

**Tech Stack:** C++17 (core, host tests with `tests/check.h`), Pico SDK 2.1.1 (RP2350, `pico2_w`), lwIP raw API, `hardware_uart`. Build lines in `README.md` and below.

**Spec:** `docs/superpowers/specs/2026-08-23-pip-wow-demo-design.md` (in `~/git/tiny_agent_cpp`). Sections "Body firmware v1" and "The link" are binding.

**Repo:** `~/git/pip` (worktree for this plan: see dispatch). Current main: `d9d6af2`.

## Global Constraints

- Framebuffer stays one 320x240 RGB565 buffer (150 KB) in `.bss`; no second buffer, no heap allocation in the frame loop.
- All drawing is dirty-rect based: every draw returns the `Rect` it touched and `Face::tick` returns the union to push. The HUD redraws only when its state changed.
- Frame budget 33 ms at 30 fps; nothing in the main loop blocks longer than one `lcd.push` of the dirty rect.
- `core/` compiles on the host with `-Wall -Wextra` warning-free and has no Pico includes. Firmware builds `pip` and `pip` with `-DPIP_LITE=ON` warning-free.
- `firmware/config.h` is git-ignored and never committed; never put WiFi credentials or IPs in source.
- HTTP bodies stay capped at 1 KB; HTTP responses carry `X-Pip-Protocol: 1`.
- Link frame: `0xA5 | type u8 | len u16 LE | payload | crc8(type,len_lo,len_hi,payload)`; type `0x01` JSON, `0x02` AUDIO; payload ≤ 512 bytes; CRC-8 poly 0x07 init 0x00 (the "CRC-8" of the crc catalogue, check value of "123456789" = 0xF4).
- UART0 on GP0 (TX) / GP1 (RX), 921600 8N1, pins from `firmware/pins.hpp` (`pins::UART_TX`, `pins::UART_RX` already exist).
- Emotion names, in this order and spelling: `idle, happy, sleepy, thinking, alert, wink, surprised, sad, listening, talking`. Chirp names unchanged: `rise, trill, drop, purr` (Plan B adds `boot, sad`; do not add them here).
- Commit after every task with a conventional message; keep `README.md` and `PROTOCOL.md` truthful in the same commit that changes behaviour.
- Build commands (Mac):
  - host tests: `cmake -S tests -B build-tests -G Ninja && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure`
  - firmware: `export PATH=$HOME/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin:$PATH PICO_SDK_PATH=$HOME/git/pico-sdk PICO_EXTRAS_PATH=$HOME/git/pico-extras; cmake -S firmware -B build-fw -G Ninja -DPIP_AUDIO_SMOKE=ON -DPICO_EXTRAS_PATH=$HOME/git/pico-extras && cmake --build build-fw`
  - lite: `cmake -S firmware -B build-fw-lite -G Ninja -DPIP_LITE=ON && cmake --build build-fw-lite`
  - flash (only the controller does this, on the real Pico): `picotool load -f -x build-fw/pip.uf2`

---

## File structure

| File | Responsibility |
|---|---|
| `core/include/pip/font5x7.hpp`, `core/src/font5x7.cpp` | 95-glyph 5x7 bitmap (ASCII 0x20..0x7E), column-major, `FONT5X7[95][5]`, bit 0 = top row |
| `core/include/pip/draw.hpp`, `core/src/draw.cpp` | `draw_text`, `text_width`, `draw_line` (thick), `draw_arc` (thick), `draw_round_rect`, `draw_disc`; all clipped, all return the touched `Rect` |
| `core/include/pip/protocol.hpp`, `core/src/protocol.cpp` | Emotion enum v1 (10), `Body` interface v1 (`say/hud/scene`), `HudUpdate`, `Senses` v1 (link counters), `apply_command()` shared dispatcher, HTTP routes v1, `X-Pip-Protocol: 1` |
| `core/include/pip/json_mini.hpp`, `core/src/json_mini.cpp` | add `get_bool` |
| `core/include/pip/face.hpp`, `core/src/face.cpp` | Face v2: layout, brows, mouth, mood tint, idle life, return-to-idle, talking, listening glyph, bubble |
| `core/include/pip/hud.hpp`, `core/src/hud.cpp` | `HudState`, `Hud::draw(fb, force)`; formats `reflex`/`judge` values |
| `core/include/pip/link.hpp`, `core/src/link.cpp` | crc8, `encode`, `Decoder` (byte-wise, resyncs) |
| `firmware/src/net/link.cpp/.hpp` | UART0 init, RX IRQ ring (4 KB), `link_poll(body, counters)` decoding in the main loop, `link_send_json`, `link_alive(now_ms)` |
| `firmware/src/body.hpp` | `RealBody` mailboxes for say/hud/scene |
| `firmware/src/main.cpp` | integrate: face v2, HUD, bubble, link, events over wire-else-HTTP, senses over wire every 500 ms, hello at boot |
| `tests/test_font.cpp`, `tests/test_draw.cpp`, `tests/test_link.cpp`, `tests/test_hud.cpp`, `tests/render_frames.cpp` | new host tests + a PPM frame dumper for visual QA |
| `tests/test_face.cpp`, `tests/test_protocol.cpp`, `tests/test_json.cpp` | extended |
| `PROTOCOL.md`, `README.md` | v1 |

---

### Task 1: 5x7 font and draw primitives

**Files:**
- Create: `core/include/pip/font5x7.hpp`, `core/src/font5x7.cpp`, `core/include/pip/draw.hpp`, `core/src/draw.cpp`
- Test: `tests/test_font.cpp`, `tests/test_draw.cpp`; add both to `tests/CMakeLists.txt` (`pip_test(test_font)`, `pip_test(test_draw)`)

**Interfaces:**
- Produces:
  ```cpp
  // font5x7.hpp
  namespace pip { extern const uint8_t FONT5X7[95][5]; // glyph i = ASCII 0x20+i, 5 columns, bit0 = top row, bit6 = bottom row
                  inline const uint8_t* glyph(char c) { unsigned i = (unsigned char)c; return (i < 0x20 || i > 0x7E) ? FONT5X7['?' - 0x20] : FONT5X7[i - 0x20]; } }
  // draw.hpp
  namespace pip {
  Rect draw_text(Framebuffer& fb, int x, int y, const char* s, int scale, uint16_t colour);   // advance 6*scale per char, no wrap
  int  text_width(const char* s, int scale);                                                  // 6*scale*len - scale (no trailing gap)
  Rect draw_line(Framebuffer& fb, int x0, int y0, int x1, int y1, uint16_t colour, int thickness); // Bresenham, each point a disc of radius thickness/2 (thickness 1 = single pixel)
  Rect draw_disc(Framebuffer& fb, int cx, int cy, int r, uint16_t colour);
  Rect draw_arc(Framebuffer& fb, int cx, int cy, int r, int start_deg, int end_deg, uint16_t colour, int thickness); // degrees clockwise from 3 o'clock (screen coords, y down); step 2 degrees
  Rect draw_round_rect(Framebuffer& fb, Rect r, int radius, uint16_t fill, uint16_t border); // border 1 px; fill == border -> solid
  }
  ```
- Consumes: `Framebuffer`, `Rect`, `rgb565` from `face.hpp` (move `Rect`, `Framebuffer`, `rgb565`, `rect_union` into a new `core/include/pip/fb.hpp` + `core/src/fb.cpp` and have `face.hpp` include it; keep the names in `namespace pip`).

The font: the classic 5x7 ASCII bitmap used by Adafruit GFX `glcdfont` / Hitachi HD44780 style. Write all 95 glyphs. Column-major: `FONT5X7['A'-0x20] = {0x7E, 0x11, 0x11, 0x11, 0x7E}`, `'I' = {0x00, 0x41, 0x7F, 0x41, 0x00}`, `' ' = {0,0,0,0,0}`, `'!' = {0x00,0x00,0x5F,0x00,0x00}`, `'0' = {0x3E,0x51,0x49,0x45,0x3E}`, `'1' = {0x00,0x42,0x7F,0x40,0x00}`, `'o' = {0x38,0x44,0x44,0x44,0x38}`, `'u' = {0x3C,0x40,0x40,0x20,0x7C}`, `'s' = {0x48,0x54,0x54,0x54,0x20}`. Lower-case glyphs descend below baseline are allowed to use bit 6 as the descender row (as glcdfont does).

- [ ] **Step 1: Write failing tests** `tests/test_font.cpp`:
  ```cpp
  #include <cstring>
  #include "check.h"
  #include "pip/font5x7.hpp"
  #include "pip/draw.hpp"
  using namespace pip;
  static Framebuffer fb;
  static void run() {
      CHECK_EQ(glyph('A')[0], (uint8_t)0x7E); CHECK_EQ(glyph('A')[4], (uint8_t)0x7E); CHECK_EQ(glyph('A')[2], (uint8_t)0x11);
      CHECK_EQ(glyph('I')[2], (uint8_t)0x7F); CHECK_EQ(glyph(' ')[2], (uint8_t)0);
      CHECK_EQ(glyph('\x01')[0], glyph('?')[0]);  // out of range -> '?'
      fb.fill(0);
      Rect r = draw_text(fb, 10, 10, "I", 1, 0xFFFF);
      CHECK_EQ(r.x, (int16_t)10); CHECK_EQ(r.y, (int16_t)10); CHECK_EQ(r.w, (int16_t)5); CHECK_EQ(r.h, (int16_t)7);
      CHECK_EQ(fb.at(12, 10), (uint16_t)0xFFFF);  // the stem, top row
      CHECK_EQ(fb.at(12, 16), (uint16_t)0xFFFF);  // the stem, bottom row
      CHECK_EQ(fb.at(10, 13), (uint16_t)0);       // column 0 middle row empty for 'I'
      fb.fill(0);
      r = draw_text(fb, 0, 0, "AB", 2, 0xFFFF);
      CHECK_EQ(r.w, (int16_t)22); CHECK_EQ(r.h, (int16_t)14);  // 2 chars: 6*2*2 - 2
      CHECK_EQ(text_width("AB", 2), 22); CHECK_EQ(text_width("", 2), 0);
      CHECK_EQ(fb.at(0, 0), (uint16_t)0); CHECK_EQ(fb.at(0, 2), (uint16_t)0xFFFF);  // 'A' col0 = 0x7E: rows 1..6 set, scale 2 -> y 2..13
      fb.fill(0); draw_text(fb, 318, 238, "XYZ", 2, 0xFFFF);  // clipped, no crash
      CHECK_EQ(fb.at(319, 239), (uint16_t)0xFFFF);
  }
  TEST_MAIN()
  ```
  `tests/test_draw.cpp`:
  ```cpp
  #include "check.h"
  #include "pip/draw.hpp"
  using namespace pip;
  static Framebuffer fb;
  static void run() {
      fb.fill(0);
      Rect r = draw_line(fb, 10, 10, 50, 10, 0xFFFF, 1);
      CHECK_EQ(fb.at(30, 10), (uint16_t)0xFFFF); CHECK_EQ(fb.at(30, 11), (uint16_t)0);
      CHECK_EQ(r.y, (int16_t)10); CHECK_EQ(r.h, (int16_t)1);
      fb.fill(0); draw_line(fb, 10, 10, 50, 10, 0xFFFF, 5);
      CHECK_EQ(fb.at(30, 8), (uint16_t)0xFFFF); CHECK_EQ(fb.at(30, 12), (uint16_t)0xFFFF); CHECK_EQ(fb.at(30, 14), (uint16_t)0);
      fb.fill(0); draw_line(fb, 0, 0, 100, 100, 0xFFFF, 1); CHECK_EQ(fb.at(50, 50), (uint16_t)0xFFFF);
      fb.fill(0); draw_disc(fb, 100, 100, 10, 0xFFFF); CHECK_EQ(fb.at(100, 109), (uint16_t)0xFFFF); CHECK_EQ(fb.at(100, 111), (uint16_t)0);
      fb.fill(0); r = draw_arc(fb, 100, 100, 30, 0, 180, 0xFFFF, 3);   // lower half (y down): smile
      CHECK_EQ(fb.at(100, 130), (uint16_t)0xFFFF); CHECK_EQ(fb.at(100, 70), (uint16_t)0);
      CHECK(r.y >= 95 && r.y + r.h <= 135);
      fb.fill(0); draw_arc(fb, 100, 100, 30, 180, 360, 0xFFFF, 3);    // upper half: frown
      CHECK_EQ(fb.at(100, 70), (uint16_t)0xFFFF); CHECK_EQ(fb.at(100, 130), (uint16_t)0);
      fb.fill(0); r = draw_round_rect(fb, Rect{10, 10, 100, 40}, 8, 0x1111, 0xFFFF);
      CHECK_EQ(fb.at(60, 30), (uint16_t)0x1111); CHECK_EQ(fb.at(60, 10), (uint16_t)0xFFFF); CHECK_EQ(fb.at(10, 10), (uint16_t)0);  // corner cut
      CHECK_EQ(r.w, (int16_t)100);
      draw_round_rect(fb, Rect{300, 220, 100, 100}, 8, 0x1111, 0xFFFF);  // clipped, no crash
  }
  TEST_MAIN()
  ```
- [ ] **Step 2: Run, verify they fail to compile** (`cmake -S tests -B build-tests -G Ninja && cmake --build build-tests`).
- [ ] **Step 3: Implement** `fb.hpp/cpp` (moved types), `font5x7.cpp` (full table), `draw.cpp`. Clipping via `Framebuffer::fill_rect`-style bounds checks; `draw_text` draws each set bit as a `scale x scale` rect. Return rects clipped to the screen.
- [ ] **Step 4: Run tests, all pass; existing tests still pass.**
- [ ] **Step 5: Commit** `feat(core): 5x7 font and draw primitives (text, thick line, arc, round rect)`.

---

### Task 2: Protocol v1 types, Body v1, shared command dispatcher, HTTP routes v1

**Files:**
- Modify: `core/include/pip/protocol.hpp`, `core/src/protocol.cpp`, `core/include/pip/json_mini.hpp`, `core/src/json_mini.cpp`, `PROTOCOL.md`
- Test: `tests/test_protocol.cpp`, `tests/test_json.cpp` (extend)

**Interfaces:**
- Produces:
  ```cpp
  enum class Emotion : uint8_t { Idle, Happy, Sleepy, Thinking, Alert, Wink, Surprised, Sad, Listening, Talking, Count };
  struct LinkStats { uint32_t rx_frames = 0, rx_bad = 0, audio_dropped = 0; };
  struct AudioStats { uint32_t free_bytes = 0; bool playing = false; };
  struct Senses { float light_lux; float temp_c; bool button_down; LinkStats link; AudioStats audio; };
  struct HudUpdate {   // every field optional; has_* says which were present
      bool has_reflex_us = false, has_judge_ms = false, has_brain = false, has_cortex = false, has_mind = false, has_scene = false;
      long reflex_us = 0, judge_ms = 0; bool brain = false, cortex = false; char mind = ' '; char scene[16] = {0};
  };
  struct Body {
      virtual ~Body() = default;
      virtual void express(Emotion e) = 0;
      virtual void chirp(Chirp c) = 0;
      virtual void led(uint8_t r, uint8_t g, uint8_t b) = 0;
      virtual void say(const char* text) = 0;      // text <= 95 chars, the caller truncates
      virtual void hud(const HudUpdate& u) = 0;
      virtual void scene(const char* name) = 0;    // <= 15 chars
      virtual Senses senses() = 0;
  };
  // Shared by HTTP and the link. cmd is "express"|"chirp"|"led"|"say"|"hud"|"scene"|"ping"; obj is the JSON object holding the args
  // (for HTTP the request body; for the link the whole frame object, whose "cmd" key is ignored here).
  // Returns 200 on success and writes a JSON reply into reply (e.g. {"ok":true} or {"pong":true}); 400/404 with {"error":"..."} otherwise.
  int apply_command(const char* cmd, const char* obj, size_t len, Body& body, char* reply, size_t cap);
  size_t senses_json(const Senses& s, char* out, size_t cap);   // {"light_lux":..,"temp_c":..,"button":"up","link":{"rx_frames":..,"rx_bad":..,"audio_dropped":..},"audio":{"free":..,"playing":false}}
  // json_mini: bool json::get_bool(const char* obj, size_t len, const char* key, bool* out);  // true/false literals only
  ```
- `handle_request` routes: `POST /express /chirp /led /say /hud /scene` via `apply_command(path+1, ...)`, `GET /senses` via `senses_json`, and `GET /ping` → `{"pong":true}`. `build_response` extra header becomes `X-Pip-Protocol: 1` everywhere (find where v0 is set and bump it).
- `/say` validates `text` present and non-empty; truncates at 95 chars. `/hud` accepts any subset of `reflex_us, judge_ms` (ints ≥ 0), `brain, cortex` (bools), `mind` (1-char string, `J` or `5` or `-`), `scene` (≤ 15 chars). `/scene` needs `name` ≤ 15 chars.

- [ ] **Step 1: Tests** in `tests/test_protocol.cpp`: a `FakeBody` that records calls; check `/say` (200 and recorded text; 400 on missing text), `/hud` with a subset (recorded `has_*` flags exactly), `/scene`, `/ping`, unknown emotion `surprised` now accepted, `/senses` JSON contains `"link":{"rx_frames":3` when the fake returns `link.rx_frames = 3`, response header carries `X-Pip-Protocol: 1`. `apply_command("hud", "{\"cmd\":\"hud\",\"judge_ms\":5800}", ...)` sets only `has_judge_ms`. `tests/test_json.cpp`: `get_bool` true/false/missing/non-bool.
- [ ] **Step 2: Run, fail.**
- [ ] **Step 3: Implement.** Keep `handle_request` as the HTTP entry; it now delegates to `apply_command`.
- [ ] **Step 4: Tests pass. Update `PROTOCOL.md` to v1** (routes, payloads, header bump, the link section from the Global Constraints with the JSON object shapes: up `{"event":...}`, `{"senses":{...}}`, `{"hello":{"fw":"v1","protocol":1}}`, `{"pong":true}`; down `{"cmd":...}`).
- [ ] **Step 5: Commit** `feat(core): protocol v1, say/hud/scene, shared command dispatcher`.

---

### Task 3: Link codec (core)

**Files:**
- Create: `core/include/pip/link.hpp`, `core/src/link.cpp`
- Test: `tests/test_link.cpp` (add `pip_test(test_link)`)

**Interfaces:**
```cpp
namespace pip::link {
constexpr uint8_t SYNC = 0xA5;
enum class Type : uint8_t { Json = 0x01, Audio = 0x02 };
constexpr size_t MAX_PAYLOAD = 512, HEADER = 4, MAX_FRAME = HEADER + MAX_PAYLOAD + 1;
uint8_t crc8(const uint8_t* p, size_t n, uint8_t crc = 0);      // poly 0x07, MSB first, init 0, no reflection, no xorout
size_t encode(Type t, const uint8_t* payload, uint16_t len, uint8_t* out, size_t cap);   // 0 if len > MAX_PAYLOAD or cap too small
struct Frame { Type type; uint16_t len; const uint8_t* payload; };
class Decoder {
public:
    bool push(uint8_t b);                 // true when a complete, CRC-valid frame is available via frame(); valid until the next push
    const Frame& frame() const { return frame_; }
    uint32_t frames() const { return frames_; }   // good frames
    uint32_t bad() const { return bad_; }         // CRC failures + oversize lengths
private:
    enum class St : uint8_t { Sync, Type, Len0, Len1, Payload, Crc } st_ = St::Sync;
    uint8_t buf_[MAX_PAYLOAD]; uint16_t len_ = 0, got_ = 0; uint8_t type_ = 0;
    Frame frame_{}; uint32_t frames_ = 0, bad_ = 0;
};
}
```
Behaviour: an unknown type byte or a len > 512 → count bad, back to Sync (the offending byte is re-examined as a possible SYNC: i.e. call the Sync check on it). CRC covers type, len_lo, len_hi, payload. On CRC mismatch count bad and return to Sync.

- [ ] **Step 1: Tests**: `crc8((uint8_t*)"123456789", 9) == 0xF4`; round trip JSON frame; round trip 512-byte audio frame; `encode` with 513 bytes returns 0; garbage prefix `{0x00, 0xFF, 0xA5, 0x09}` then a valid frame decodes (bad count 1 because 0xA5 0x09 is an unknown type); corrupt one payload byte → bad == 1, frames == 0, and a following valid frame decodes; two back-to-back frames both decode.
- [ ] **Step 2-5:** fail, implement, pass, commit `feat(core): link frame codec with crc8 and resync`.

---

### Task 4: Face v2 layout, brows, mouth, mood tint, idle life, return-to-idle, talking, listening

**Files:**
- Modify: `core/include/pip/face.hpp`, `core/src/face.cpp`
- Test: `tests/test_face.cpp` (extend), `tests/render_frames.cpp` (new, not a test: dumps one PPM per emotion plus bubble/HUD frames to `./frames/` so the controller can eyeball them; add as an executable target `render_frames`, not via `pip_test`)

**Interfaces:**
```cpp
struct BrowShape { int16_t angle_deg; int16_t raise; };          // angle: negative = inner end down (angry/sad), positive = inner end up; raise: px above default
enum class MouthKind : uint8_t { Flat, Smile, Frown, O, Open };
struct MouthShape { MouthKind kind; int16_t width; int16_t open_pct; }; // open_pct used by Open (talking) 0..100
struct FaceShape { EyeShape left, right; BrowShape lbrow, rbrow; MouthShape mouth; uint16_t bg; };
FaceShape shape_for(Emotion e);
uint16_t mood_bg(Emotion e);   // idle rgb565(12,12,28); happy (40,24,8); sleepy (4,4,8); thinking (24,10,40); alert (48,28,0); wink = idle; surprised (30,30,40); sad (14,18,30); listening (8,24,32); talking = previous emotion's bg
class Face {
public:
    static constexpr int FACE_H = 200;                 // face region; HUD lives below
    static constexpr int LEFT_CX = 110, RIGHT_CX = 210, EYE_CY = 88, MOUTH_CY = 165, BROW_DY = -62;
    static constexpr uint16_t EYE = rgb565(240,240,255), PUPIL = rgb565(20,20,40), LINE = rgb565(230,230,240);
    void set_emotion(Emotion e);                       // starts the hold timer for timed emotions
    void set_night(bool n);                            // expiry goes to Sleepy instead of Idle while night
    void set_talking(bool on);                         // overlays Talking (mouth animates) and restores the previous emotion when off
    void set_listening_level(uint8_t pct);             // optional: waveform glyph amplitude, default animates on its own
    void say(const char* text, uint32_t hold_ms);      // bubble; text wrapped to 2 lines of <= 24 chars at scale 2; hides the mouth while shown
    void clear_say();
    bool bubble_visible() const;
    Emotion emotion() const;                           // target emotion (Talking reported while talking)
    Rect tick(uint32_t dt_ms, Framebuffer& fb);        // returns the dirty rect within the face region (y < FACE_H)
    const FaceShape& current() const;
    static uint32_t hold_ms_for(Emotion e);            // wink/alert/surprised 3000; happy/sad 10000; others 0 (= hold until changed)
};
```
Rules:
- Layout: eyes rx 40 ry 50 centred at EYE_CY 88 → rows 38..138; brows: a thick (4 px) line of half-width 34 centred on each eye at y = EYE_CY + BROW_DY (26) plus `raise`, rotated by `angle_deg` (inner end moves); mouth centred at (160, MOUTH_CY): Flat = thick line of `width`; Smile = arc r = width/2, 20..160 deg; Frown = arc 200..340 deg; O = ring (disc radius 12 of LINE with inner disc of bg radius 7); Open = round rect width x (4 + open_pct*20/100) of LINE.
- Per-emotion shapes: Idle flat mouth 40, brows 0/0. Happy: eyes as now (squint), Smile 60, brows +4 raise. Sleepy: lids 60, Flat 30, brows raise -2. Thinking: pupils up-right, Flat 30, left brow +15 deg raise 6. Alert: eyes bigger, O mouth, brows raise 10. Wink: left shut, Smile 50. Surprised: eyes rx 46 ry 60, pupils small 9, O mouth, brows raise 14. Sad: lids 35 from top, brows -18 deg, Frown 50, pupils dy +6. Listening: eyes rx 40 ry 46 pupils centred, Flat 30, brows raise 6, plus the waveform glyph (three vertical bars at x 282,292,302, y centred 88, heights animating 6..26 px in a 400 ms cycle). Talking: inherits the previous emotion's eyes/brows/bg; mouth Open with open_pct alternating 100/20 every 120 ms.
- Tween as today (`step`), including brows and mouth width; `bg` switches instantly (full face-region repaint when bg changes).
- Idle life: blink as now (only in Idle/Happy/Sleepy/Listening; Sleepy gaps ×2); saccade: in Idle/Happy/Sleepy, every 2000 + (n*733)%3000 ms shift both pupils by (±6, ±3) for 300 ms then back (deterministic pseudo-random from a counter like the blink).
- Return to idle: `set_emotion` stores `hold_until = t + hold_ms_for(e)` when non-zero; `tick` returns to Idle (or Sleepy when night) when expired. `set_night(true)` while Idle switches to Sleepy; `set_night(false)` while Sleepy switches to Idle.
- Bubble: `Rect{8, 142, 304, 52}` round rect radius 8, fill rgb565(245,245,250), border rgb565(90,90,120); text colour rgb565(20,20,40), scale 2, line 1 at y 148, line 2 at y 166, left x 16, word wrap at 24 chars (break on spaces, hard-break a longer word); hold timer; while visible the mouth is not drawn and its rect is repainted with bg.
- Dirty rects: union of old/new bounds of every part that changed (eyes, brows, mouth, glyph, bubble). Keep `first_` full repaint; bg change → `Rect{0,0,320,FACE_H}`.

- [ ] **Step 1: Tests** (extend `test_face.cpp`): for every emotion `set_emotion`, `settle`, `tick` → no writes at y ≥ FACE_H (check a row at y=200 stays 0 after `fb.fill(0)` pre-pass; simplest: fill rows 200..239 with 0x0BAD before, assert unchanged after); `hold_ms_for` table values; Wink returns to Idle after 3000 ms of ticks and to Sleepy when `set_night(true)`; Happy bubble: `say("hello world", 3000)` → pixel at (60,160) is the fill colour, after 3000 ms ticks it is the bg again; `set_talking(true)` then ticks → `emotion()==Talking`, mouth pixel at (160, MOUTH_CY) toggles between LINE and bg across 240 ms; `set_talking(false)` restores Happy; saccade moves the pupil: after ~2.3 s of idle ticks `current().left.pupil_dx != 0` at some tick (loop 150 ticks of 16 ms and record whether any tick had a nonzero offset); smile pixel check: Happy settled → `fb.at(160, MOUTH_CY + 28)` is LINE (bottom of the smile arc r=30) and Sad → `fb.at(160, MOUTH_CY - 23)` is LINE.
- [ ] **Step 2: fail. Step 3: implement. Step 4: pass; run `render_frames` and look at `frames/*.ppm` converted to PNG (`python3 -c` with PIL, or `sips`), fix anything ugly (brows touching eyes, arcs off-centre).**
- [ ] **Step 5: Commit** `feat(core): face v2, brows, mouth, moods, idle life, return-to-idle, bubble, talking`.

---

### Task 5: HUD strip

**Files:**
- Create: `core/include/pip/hud.hpp`, `core/src/hud.cpp`
- Test: `tests/test_hud.cpp`

**Interfaces:**
```cpp
struct HudState {
    float lux = -1; bool night = false; float temp_c = 0;
    long reflex_us = -1, judge_ms = -1;          // -1 = never
    bool wire = false, wifi = false, brain = false, cortex = false; char mind = '-';
    char scene[16] = {0};
};
class Hud {
public:
    static constexpr int Y0 = 200, H = 40;
    static constexpr uint16_t BG = rgb565(6,6,12), FG = rgb565(200,200,215), DIM = rgb565(60,60,80), OK = rgb565(60,220,120), WARN = rgb565(240,180,40);
    void apply(const HudUpdate& u);              // merges has_* fields
    void set_senses(float lux, bool night, float temp_c, bool wire, bool wifi);
    const HudState& state() const;
    Rect draw(Framebuffer& fb, bool force);      // full strip repaint when anything changed or force; else empty Rect
    static void fmt_us(long us, char* out, size_t cap);    // "95us", "1.2ms", "--"
    static void fmt_ms(long ms, char* out, size_t cap);    // "5.8s", "850ms", "--"
};
```
Layout (scale 2 text, 14 px tall): top line y 203: scene caption at x 6 (FG, or DIM "idle" when empty); right-aligned glyph string at x 320-6-width: `W F B C` each drawn OK when up / DIM when down, then mind char in FG (`J`/`5`/`-`). Bottom line y 222: lux bar x 6..56 (50 px wide, 10 px tall, border DIM, fill OK proportional to log10(1+lux)/log10(201) clamped; moon glyph = a small disc of FG at x 62 when night); text at x 76: `"%2.0fC rfx %s jdg %s"`. Repaint: whole strip `Rect{0,200,320,40}` only when `memcmp(state)` changed since last draw.

- [ ] **Step 1: Tests**: `fmt_us(95)=="95us"`, `fmt_us(1234)=="1.2ms"`, `fmt_us(-1)=="--"`, `fmt_ms(5800)=="5.8s"`, `fmt_ms(850)=="850ms"`; first `draw` returns the full strip; second `draw` with no change returns empty; `apply` with `has_judge_ms` changes state and `draw` returns the strip again; pixel checks: after `set_senses(200, false, 25, true, ...)` the bar's right end `fb.at(54, 227)` is OK, after lux 0 it is BG; night → moon pixel `fb.at(62, 227)` is FG.
- [ ] **Steps 2-5:** fail, implement, pass, commit `feat(core): HUD strip with lux bar, reflex/judge headline, link glyphs`.

---

### Task 6: Firmware UART link transport, mailboxes, main-loop integration

**Files:**
- Create: `firmware/src/net/link.hpp`, `firmware/src/net/link.cpp`
- Modify: `firmware/src/body.hpp`, `firmware/src/main.cpp`, `firmware/CMakeLists.txt` (add `src/net/link.cpp`, link `hardware_uart` + `hardware_irq`), `README.md`

**Interfaces:**
```cpp
namespace pip::net {
void link_init(unsigned baud);                                  // uart0 on pins::UART_TX/RX, RX IRQ -> 4 KB ring
// Drains the ring into a link::Decoder; for each JSON frame calls apply_command(cmd,...) on body (cmd from json::get_string "cmd");
// a {"cmd":"ping"} gets {"pong":true} sent back; AUDIO frames are counted (Plan B consumes them) via on_audio when non-null.
void link_poll(Body& body, void (*on_audio)(const uint8_t* pcm, uint16_t len));
bool link_send_json(const char* json);                          // encodes + uart_write_blocking; false if too long
bool link_alive(uint32_t now_ms);                               // a good frame was seen within the last 2000 ms
LinkStats link_stats();
}
```
- `RealBody` gains mailboxes: `say` (char[96] + atomic flag), `hud` (HudUpdate + atomic flag; successive updates before the main loop drains merge their `has_*` fields), `scene` (char[16] + flag), and `publish` takes `LinkStats` + `AudioStats` too (AudioStats zeros in this plan).
- `main.cpp`: 
  - boot: `link_init(921600)`; send `{"hello":{"fw":"v1","protocol":1}}`.
  - each frame: `link_poll(body, nullptr)`; drain mailboxes → `face.set_emotion`, `face.say(text, 3000)`, `hud.apply(u)`, scene → `hud` scene field; `hud.set_senses(lux, light.is_low(), temp_c, link_alive(now), online)`; `face.set_night(light.is_low())`.
  - every 500 ms: senses over the link when alive: `{"senses":{"light_lux":..,"temp_c":..,"button":"up"}}` (the same JSON as `/senses` wrapped).
  - events: `if (link_alive(now)) link_send_json({"event":..}) else if (online) post_event(...)`.
  - render: `Rect d = face.tick(dt, fb); Rect h = hud.draw(fb, false); push d then h` (two pushes when both non-empty).
  - keep the LED press-borrow behaviour and the existing prints.
- `pip-lite` must still build: no display → `hud`/`face` still run against the framebuffer, nothing is pushed.

- [ ] **Step 1:** Build both variants; fix warnings.
- [ ] **Step 2:** On the bench (controller does this): flash, `curl -s http://192.168.1.110/senses` shows `link.rx_frames`; from the Zero `python3 - <<EOF` (a 20-line script using `pyserial`-free `os.open` + the frame format) send `{"cmd":"express","emotion":"happy"}` and `{"cmd":"say","text":"hello from the wire"}` and `{"cmd":"hud","scene":"reflex","reflex_us":95,"judge_ms":5800,"brain":true}`; the screen shows the face, the bubble and the HUD; `{"cmd":"ping"}` returns a `{"pong":true}` frame. Put that script in `brain/scripts/link-probe.py` (it doubles as the bench tool).
- [ ] **Step 3:** README: link section (what travels on the wire, what stays on HTTP, how to probe), screen layout picture (ASCII), the new routes.
- [ ] **Step 4: Commit** `feat(firmware): UART link to the brain, face v2 + HUD + bubble on screen, v1 routes`.

---

### Task 7: Hardware doc touch

**Files:** `hardware/pip-pictorial.py` caption only if the pin story changed (it did not; skip unless a pin moved), `README.md` hardware table: mark the link as live.

- [ ] Commit with Task 6 if trivial.

---

## Self-review notes
- Spec coverage: screen v2 ✔ (T4, T5), font/primitives ✔ (T1), bubble ✔ (T4), HUD ✔ (T5), link codec + transport + events-over-wire + senses over wire + hello/pong ✔ (T3, T6), v1 routes + header ✔ (T2), `/senses` link counters ✔ (T2/T6), audio fields present but zero until Plan B ✔. Chirp synth and PCM ring are Plan B.
- Type consistency: `HudUpdate` (protocol.hpp) is consumed by `Hud::apply` and `RealBody`; `LinkStats`/`AudioStats` in `Senses`; `Face::FACE_H == Hud::Y0`.
