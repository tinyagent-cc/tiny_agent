# Pip Firmware (Body) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Pico 2 W firmware that is Pip's body: an animated eyes-first face on the ILI9341, senses (light, temp, button), an HTTP/JSON tool server implementing `PROTOCOL.md` v0, and event POSTs to the brain; plus a `pip-lite` build for a bare Pico 2 W and an I2S smoke that settles whether `pico_audio_i2s` runs on RP2350 before audio gets designed.

**Architecture:** Two layers. `core/` is platform-free C++17 (framebuffer + face engine, JSON mini-codec, HTTP parsing, protocol routing, event state machines) compiled and unit-tested on the Mac with no SDK. `firmware/` is the Pico SDK glue: drivers (ILI9341 over SPI, VEML7700 over I2C, RGB LED over PWM, button, temp ADC), WiFi, a raw-lwIP-tcp HTTP server, a raw-tcp one-shot event POST, and `main.cpp` tying them together at 30 fps. lwIP's bundled httpd/http_client are not used (file-shaped POST, no client POST).

**Tech Stack:** Pico SDK 2.1.1 (`~/git/pico-sdk`, `PICO_BOARD=pico2_w`, `pico_cyw43_arch_lwip_threadsafe_background`), pico-extras (`~/git/pico-extras`, I2S smoke only), ARM GNU Toolchain 14.2 (`PICO_TOOLCHAIN_PATH=$HOME/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin`), CMake + Ninja, picotool 2.3.0, C++17, host tests with a 20-line `check.h`.

**Spec:** `docs/superpowers/specs/2026-08-22-pip-companion-design.md` (Track 1, body + interfaces) and `~/git/pip/PROTOCOL.md` (v0 contract). API facts scouted from source on 2026-08-22 (file:line cites) are in the session scratchpad `pico-api-facts.md`; the signatures used below were copied from it.

## Global Constraints

- Repo: `~/git/pip` (github.com/tinyagent-cc/pip, branch `main`, currently README + PROTOCOL + LICENSE + .gitignore). Work on branch `feat/firmware-v0`.
- Env for every device build: `export PICO_SDK_PATH=$HOME/git/pico-sdk PICO_EXTRAS_PATH=$HOME/git/pico-extras PICO_TOOLCHAIN_PATH=$HOME/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin`. Do not use Homebrew's `arm-none-eabi-gcc` (no newlib).
- Device build: `cmake -S firmware -B build-fw -G Ninja && cmake --build build-fw`. Host tests: `cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure`. Both must be green before every commit that touches them.
- Flash: board in BOOTSEL mounts `/Volumes/RP2350`; `picotool load -x build-fw/pip.uf2`. Board already running Pip: `picotool load -f -x build-fw/pip.uf2`. Serial: `/dev/cu.usbmodem1301`, read with the python raw-tty loop (`cat` misses it).
- Pins (fixed for this plan; `firmware/pins.hpp` is the single source): ILI9341 on SPI0: SCK GP18, MOSI GP19, CS GP17, DC GP20, RST GP21, backlight to 3V3. VEML7700 on I2C0: SDA GP4, SCL GP5. Button GP15 to GND (internal pull-up). RGB LED: R GP10, G GP11, B GP12 through resistors, common cathode to GND (`PIP_RGB_COMMON_ANODE` flips it). I2S (smoke only): BCLK GP26, LRCLK GP27, DIN GP28 (pico_audio_i2s defaults).
- Secrets: WiFi SSID/password and brain URL live only in `firmware/config.h`, generated from `firmware/config.example.h`; `config.h` is git-ignored already. Never commit it, never echo it into a report.
- Protocol: exactly `~/git/pip/PROTOCOL.md` v0. Every response carries `X-Pip-Protocol: 0`. Unknown names return 400 `{"error":"..."}`.
- C++17, `-Wall -Wextra` clean on host. No heap in lwIP callbacks; no blocking calls in lwIP callbacks; any lwIP raw-API call from the main loop is bracketed with `cyw43_arch_lwip_begin()`/`cyw43_arch_lwip_end()`.
- Prose a human reads (README, comments that explain) follows `~/.agents/skills/humanize/SKILL.md` and `~/git/mystaff/.claude/skills/posture/SKILL.md`; zero em dashes anywhere, code comments included.
- Every commit ends with the two-line trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01H7hNgsKhpDyCMmrMbSLSfi`.
- Bench steps that need eyes on hardware (display shows eyes, LED colour, sound) cannot be verified by a subagent; report them as `DONE_WITH_CONCERNS: bench unverified` with the exact thing Riadh must look at. Everything else is verified with tool output.

---

### Task 1: Repo layout, host test harness, firmware skeleton, CI

**Files:**
- Create: `tests/check.h`, `tests/CMakeLists.txt`, `tests/test_smoke.cpp`
- Create: `firmware/CMakeLists.txt`, `firmware/pico_sdk_import.cmake` (copy of `~/git/pico-sdk/external/pico_sdk_import.cmake`), `firmware/pins.hpp`, `firmware/lwipopts.h`, `firmware/src/main.cpp`
- Create: `.github/workflows/host-tests.yml`
- Modify: `.gitignore` (add `build-*/`)

**Interfaces:**
- Produces: the build shapes every later task extends; `pins.hpp` constants; the `CHECK`/`CHECK_EQ` macros.

- [ ] **Step 1: Host test harness**

`tests/check.h`:
```cpp
#pragma once
#include <cstdio>
static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fails; std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; if (!((a) == (b))) { ++g_fails; std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define CHECK_STREQ(a, b) do { ++g_checks; if (std::strcmp((a), (b)) != 0) { ++g_fails; std::fprintf(stderr, "%s:%d: CHECK_STREQ failed: '%s' vs '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)
#define TEST_MAIN() int main() { run(); std::printf("%d checks, %d failed\n", g_checks, g_fails); return g_fails ? 1 : 0; }
```

`tests/test_smoke.cpp`:
```cpp
#include <cstring>
#include "check.h"
static void run() { CHECK(1 + 1 == 2); CHECK_STREQ("pip", "pip"); }
TEST_MAIN()
```

`tests/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.20)
project(pip_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)
enable_testing()
set(PIP_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core)
file(GLOB PIP_CORE_SRC ${PIP_CORE_DIR}/src/*.cpp)
add_library(pip_core STATIC ${PIP_CORE_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/empty.cpp)
target_include_directories(pip_core PUBLIC ${PIP_CORE_DIR}/include)
function(pip_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} pip_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()
pip_test(test_smoke)
```
Create `tests/empty.cpp` containing only a comment (`// keeps pip_core a valid library before core/ has sources`). Create `core/include/pip/.gitkeep` and `core/src/.gitkeep` so the glob paths exist.

- [ ] **Step 2: Run host tests**

Run: `cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure`
Expected: `1/1 tests passed`, output `2 checks, 0 failed`.

- [ ] **Step 3: Firmware skeleton**

`firmware/pins.hpp`:
```cpp
#pragma once
// Single source of truth for wiring. README's table is generated from this by hand; keep them equal.
namespace pip::pins {
constexpr unsigned SPI_SCK = 18, SPI_MOSI = 19, LCD_CS = 17, LCD_DC = 20, LCD_RST = 21;
constexpr unsigned I2C_SDA = 4, I2C_SCL = 5;
constexpr unsigned BUTTON = 15;
constexpr unsigned LED_R = 10, LED_G = 11, LED_B = 12;
constexpr unsigned I2S_BCLK = 26, I2S_LRCLK = 27, I2S_DIN = 28;
}
```

`firmware/lwipopts.h` (the canonical pico-examples set; if the SDK's lwIP headers reject a macro, fix the macro and note it in the report):
```c
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H
#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 4000
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
#define PBUF_POOL_SIZE 24
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1
#define TCP_WND (8 * TCP_MSS)
#define TCP_MSS 1460
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETCONN 0
#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#ifndef NDEBUG
#define LWIP_DEBUG 1
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 1
#endif
#define ETHARP_DEBUG LWIP_DBG_OFF
#define NETIF_DEBUG LWIP_DBG_OFF
#define PBUF_DEBUG LWIP_DBG_OFF
#define API_LIB_DEBUG LWIP_DBG_OFF
#define API_MSG_DEBUG LWIP_DBG_OFF
#define SOCKETS_DEBUG LWIP_DBG_OFF
#define ICMP_DEBUG LWIP_DBG_OFF
#define INET_DEBUG LWIP_DBG_OFF
#define IP_DEBUG LWIP_DBG_OFF
#define IP_REASS_DEBUG LWIP_DBG_OFF
#define RAW_DEBUG LWIP_DBG_OFF
#define MEM_DEBUG LWIP_DBG_OFF
#define MEMP_DEBUG LWIP_DBG_OFF
#define SYS_DEBUG LWIP_DBG_OFF
#define TCP_DEBUG LWIP_DBG_OFF
#define TCP_INPUT_DEBUG LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG LWIP_DBG_OFF
#define TCP_RTO_DEBUG LWIP_DBG_OFF
#define TCP_CWND_DEBUG LWIP_DBG_OFF
#define TCP_WND_DEBUG LWIP_DBG_OFF
#define TCP_FR_DEBUG LWIP_DBG_OFF
#define TCP_QLEN_DEBUG LWIP_DBG_OFF
#define TCP_RST_DEBUG LWIP_DBG_OFF
#define UDP_DEBUG LWIP_DBG_OFF
#define TCPIP_DEBUG LWIP_DBG_OFF
#define PPP_DEBUG LWIP_DBG_OFF
#define SLIP_DEBUG LWIP_DBG_OFF
#define DHCP_DEBUG LWIP_DBG_OFF
#endif
```

`firmware/src/main.cpp` (skeleton; later tasks replace it):
```cpp
#include <cstdio>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pins.hpp"

int main() {
    stdio_init_all();
    if (cyw43_arch_init() != 0) { printf("pip: cyw43 init failed\n"); return 1; }
    printf("pip body v0 skeleton, button pin %u\n", pip::pins::BUTTON);
    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(100);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(900);
    }
}
```

`firmware/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pico2_w)
include(pico_sdk_import.cmake)
project(pip C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

option(PIP_LITE "Bare Pico 2 W: no display, light sensor, or RGB LED" OFF)
option(PIP_AUDIO_SMOKE "Also build the I2S tone smoke (needs PICO_EXTRAS_PATH)" OFF)

set(PIP_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../core)
file(GLOB PIP_CORE_SRC ${PIP_CORE}/src/*.cpp)

add_executable(pip src/main.cpp ${PIP_CORE_SRC})
target_include_directories(pip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIP_CORE}/include)
target_link_libraries(pip pico_stdlib pico_cyw43_arch_lwip_threadsafe_background hardware_adc)
if(PIP_LITE)
  target_compile_definitions(pip PRIVATE PIP_LITE=1)
else()
  target_link_libraries(pip hardware_spi hardware_i2c hardware_pwm)
endif()
pico_enable_stdio_usb(pip 1)
pico_enable_stdio_uart(pip 0)
pico_add_extra_outputs(pip)
```

`.github/workflows/host-tests.yml`:
```yaml
name: host-tests
on: [push, pull_request, workflow_dispatch]
jobs:
  host:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: cmake -S tests -B build-host
      - run: cmake --build build-host
      - run: ctest --test-dir build-host --output-on-failure
```

Append `build-*/` to `.gitignore`.

- [ ] **Step 4: Build firmware both ways**

Run (env exported per Global Constraints): `cmake -S firmware -B build-fw -G Ninja && cmake --build build-fw && ls -la build-fw/pip.uf2`
Expected: `pip.uf2` exists. Then `cmake -S firmware -B build-fw-lite -G Ninja -DPIP_LITE=ON && cmake --build build-fw-lite` also succeeds.

- [ ] **Step 5: Flash and read serial**

Run: `picotool load -f -x build-fw/pip.uf2` (the board currently runs the smoke firmware, so `-f`), then read `/dev/cu.usbmodem1301` for 4 s with the python raw-tty loop.
Expected: `pip body v0 skeleton, button pin 15` once (stdio prints at boot; if you connect late you see nothing, which is fine: reflash and read immediately, or accept a clean flash + LED blink as proof).

- [ ] **Step 6: Commit**

```bash
git checkout -b feat/firmware-v0
git add tests core firmware .github .gitignore
git commit -m "build: host test harness, firmware skeleton, pins, lwipopts, CI

<trailer>"
```

### Task 2: JSON mini-codec (core)

**Files:**
- Create: `core/include/pip/json_mini.hpp`, `core/src/json_mini.cpp`, `tests/test_json.cpp`
- Modify: `tests/CMakeLists.txt` (add `pip_test(test_json)`)

**Interfaces:**
- Produces: `pip::json::get_string(obj,len,key,out,cap)`, `pip::json::get_int(obj,len,key,&long)`; both return false on absent key or wrong shape. Flat objects only.

- [ ] **Step 1: Failing tests**

`tests/test_json.cpp`:
```cpp
#include <cstring>
#include "check.h"
#include "pip/json_mini.hpp"
using namespace pip::json;
static void run() {
    const char* o = R"({"emotion": "happy", "r":12,"g": 0 , "b":-3, "name":"x\"y"})";
    size_t n = std::strlen(o);
    char s[16]; long v;
    CHECK(get_string(o, n, "emotion", s, sizeof s)); CHECK_STREQ(s, "happy");
    CHECK(get_int(o, n, "r", &v)); CHECK_EQ(v, 12L);
    CHECK(get_int(o, n, "g", &v)); CHECK_EQ(v, 0L);
    CHECK(get_int(o, n, "b", &v)); CHECK_EQ(v, -3L);
    CHECK(!get_int(o, n, "emotion", &v));          // string where int expected
    CHECK(!get_string(o, n, "r", s, sizeof s));      // int where string expected
    CHECK(!get_string(o, n, "missing", s, sizeof s));
    CHECK(!get_string(o, n, "name", s, sizeof s));   // escapes unsupported -> false, never garbage
    CHECK(!get_string(o, n, "emotion", s, 4));       // does not fit -> false
    const char* trick = R"({"a":"emotion","emotion":"wink"})";
    CHECK(get_string(trick, std::strlen(trick), "emotion", s, sizeof s)); CHECK_STREQ(s, "wink");
    const char* trunc = R"({"emotion":"hap)";
    CHECK(!get_string(trunc, std::strlen(trunc), "emotion", s, sizeof s));
}
TEST_MAIN()
```

- [ ] **Step 2: Run, expect compile failure (header missing)**

- [ ] **Step 3: Implement**

`core/include/pip/json_mini.hpp`:
```cpp
#pragma once
#include <cstddef>
namespace pip::json {
// Flat-object getters for Pip's tiny bodies. Keys are top-level; values are a string
// without escapes or a decimal integer. Return false when absent or the wrong shape.
bool get_string(const char* obj, size_t len, const char* key, char* out, size_t out_cap);
bool get_int(const char* obj, size_t len, const char* key, long* out);
}
```

`core/src/json_mini.cpp`:
```cpp
#include "pip/json_mini.hpp"
#include <cstring>
namespace pip::json {
namespace {
bool ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
// Returns a pointer to the first byte of the value for "key": ..., or nullptr.
const char* find_value(const char* obj, size_t len, const char* key) {
    size_t klen = std::strlen(key);
    const char* end = obj + len;
    for (const char* p = obj; p + klen + 2 <= end; ++p) {
        if (p[0] != '"' || std::memcmp(p + 1, key, klen) != 0 || p[klen + 1] != '"') continue;
        const char* q = p + klen + 2;
        while (q < end && ws(*q)) ++q;
        if (q >= end || *q != ':') continue;
        ++q;
        while (q < end && ws(*q)) ++q;
        return q < end ? q : nullptr;
    }
    return nullptr;
}
}
bool get_string(const char* obj, size_t len, const char* key, char* out, size_t cap) {
    const char* v = find_value(obj, len, key);
    if (!v || *v != '"' || cap == 0) return false;
    const char* end = obj + len;
    ++v;
    size_t n = 0;
    while (v < end && *v != '"') {
        if (*v == '\\' || n + 1 >= cap) return false;
        out[n++] = *v++;
    }
    if (v >= end) return false;
    out[n] = '\0';
    return true;
}
bool get_int(const char* obj, size_t len, const char* key, long* out) {
    const char* v = find_value(obj, len, key);
    if (!v) return false;
    const char* end = obj + len;
    bool neg = false;
    if (*v == '-') { neg = true; ++v; }
    if (v >= end || *v < '0' || *v > '9') return false;
    long acc = 0;
    while (v < end && *v >= '0' && *v <= '9') { acc = acc * 10 + (*v - '0'); ++v; }
    *out = neg ? -acc : acc;
    return true;
}
}
```
Add `pip_test(test_json)` to `tests/CMakeLists.txt`; delete `tests/empty.cpp` and the two `.gitkeep` files now that `core/src` has a real source (update the `add_library` line to drop `empty.cpp`).

- [ ] **Step 4: Run host tests**

Expected: `2/2 tests passed`; test_json prints `16 checks, 0 failed`.

- [ ] **Step 5: Commit** `feat(core): json mini-codec for flat request bodies`

### Task 3: HTTP request parsing and response building (core)

**Files:**
- Create: `core/include/pip/http.hpp`, `core/src/http.cpp`, `tests/test_http.cpp`
- Modify: `tests/CMakeLists.txt` (`pip_test(test_http)`)

**Interfaces:**
- Produces: `pip::http::Parse {Incomplete, Complete, Bad}`, `pip::http::Request {method[8], path[64], body, body_len}`, `parse_request(buf,len,Request&)`, `build_response(out,cap,status,json_body,extra_header_or_null) -> bytes`, `reason(status)`.

- [ ] **Step 1: Failing tests**

`tests/test_http.cpp`:
```cpp
#include <cstring>
#include "check.h"
#include "pip/http.hpp"
using namespace pip::http;
static void run() {
    Request r{};
    const char* get = "GET /senses HTTP/1.1\r\nHost: pip\r\n\r\n";
    CHECK(parse_request(get, std::strlen(get), r) == Parse::Complete);
    CHECK_STREQ(r.method, "GET"); CHECK_STREQ(r.path, "/senses"); CHECK_EQ(r.body_len, (size_t)0);

    const char* post = "POST /express HTTP/1.0\r\ncontent-length: 19\r\nContent-Type: application/json\r\n\r\n{\"emotion\":\"happy\"}";
    CHECK(parse_request(post, std::strlen(post), r) == Parse::Complete);
    CHECK_STREQ(r.method, "POST"); CHECK_STREQ(r.path, "/express");
    CHECK_EQ(r.body_len, (size_t)19); CHECK(std::memcmp(r.body, "{\"emotion\":\"happy\"}", 19) == 0);

    CHECK(parse_request(post, std::strlen(post) - 5, r) == Parse::Incomplete);   // body short
    CHECK(parse_request(post, 10, r) == Parse::Incomplete);                      // headers unfinished
    const char* bad = "GARBAGE\r\n\r\n";
    CHECK(parse_request(bad, std::strlen(bad), r) == Parse::Bad);
    const char* huge = "POST /x HTTP/1.0\r\nContent-Length: 99999\r\n\r\n";
    CHECK(parse_request(huge, std::strlen(huge), r) == Parse::Bad);

    char out[256];
    size_t n = build_response(out, sizeof out, 200, "{\"ok\":true}", nullptr);
    CHECK(n > 0); out[n] = 0;
    CHECK(std::strstr(out, "HTTP/1.0 200 OK\r\n") == out);
    CHECK(std::strstr(out, "Content-Type: application/json\r\n"));
    CHECK(std::strstr(out, "Content-Length: 11\r\n"));
    CHECK(std::strstr(out, "X-Pip-Protocol: 0\r\n"));
    CHECK(std::strstr(out, "Connection: close\r\n\r\n{\"ok\":true}"));
    n = build_response(out, sizeof out, 400, "{\"error\":\"x\"}", "X-Extra: 1");
    out[n] = 0; CHECK(std::strstr(out, "400 Bad Request")); CHECK(std::strstr(out, "X-Extra: 1\r\n"));
    CHECK_EQ(build_response(out, 8, 200, "{}", nullptr), (size_t)0);           // does not fit -> 0
}
TEST_MAIN()
```

- [ ] **Step 2: Run, expect compile failure**

- [ ] **Step 3: Implement**

`core/include/pip/http.hpp`:
```cpp
#pragma once
#include <cstddef>
namespace pip::http {
enum class Parse { Incomplete, Complete, Bad };
struct Request {
    char method[8];
    char path[64];
    const char* body;   // points into the caller's buffer
    size_t body_len;
};
// Parses one HTTP/1.x request. Complete once the header block ended and Content-Length
// bytes of body are present. Bodies over 1024 bytes and header blocks over 2048 are Bad.
Parse parse_request(const char* buf, size_t len, Request& out);
// Writes a complete HTTP/1.0 JSON response; returns bytes written, 0 if it did not fit.
// extra_header, when non-null, is one "Name: value" line without CRLF.
size_t build_response(char* out, size_t cap, int status, const char* json_body, const char* extra_header);
const char* reason(int status);
}
```

`core/src/http.cpp`:
```cpp
#include "pip/http.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
namespace pip::http {
namespace {
const char* find(const char* hay, size_t len, const char* needle) {
    size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= len; ++i)
        if (std::memcmp(hay + i, needle, n) == 0) return hay + i;
    return nullptr;
}
// Case-insensitive header lookup inside [hdr, hdr_end). Returns the value start or nullptr.
const char* header_value(const char* hdr, const char* hdr_end, const char* name) {
    size_t n = std::strlen(name);
    const char* line = hdr;
    while (line < hdr_end) {
        const char* eol = find(line, (size_t)(hdr_end - line), "\r\n");
        if (!eol) eol = hdr_end;
        if ((size_t)(eol - line) > n && line[n] == ':' && strncasecmp(line, name, n) == 0) {
            const char* v = line + n + 1;
            while (v < eol && *v == ' ') ++v;
            return v;
        }
        line = eol + 2;
    }
    return nullptr;
}
}
Parse parse_request(const char* buf, size_t len, Request& out) {
    const char* hdr_end = find(buf, len, "\r\n\r\n");
    if (!hdr_end) return len > 2048 ? Parse::Bad : Parse::Incomplete;
    const char* sp1 = static_cast<const char*>(std::memchr(buf, ' ', (size_t)(hdr_end - buf)));
    if (!sp1) return Parse::Bad;
    const char* sp2 = static_cast<const char*>(std::memchr(sp1 + 1, ' ', (size_t)(hdr_end - sp1 - 1)));
    if (!sp2) return Parse::Bad;
    size_t ml = (size_t)(sp1 - buf), pl = (size_t)(sp2 - sp1 - 1);
    if (ml == 0 || ml >= sizeof out.method || pl == 0 || pl >= sizeof out.path) return Parse::Bad;
    std::memcpy(out.method, buf, ml); out.method[ml] = '\0';
    std::memcpy(out.path, sp1 + 1, pl); out.path[pl] = '\0';
    const char* first_eol = find(buf, (size_t)(hdr_end - buf) + 2, "\r\n");
    const char* hdrs = first_eol ? first_eol + 2 : hdr_end;
    size_t clen = 0;
    if (const char* cl = header_value(hdrs, hdr_end, "Content-Length")) clen = std::strtoul(cl, nullptr, 10);
    if (clen > 1024) return Parse::Bad;
    const char* body = hdr_end + 4;
    size_t have = len - (size_t)(body - buf);
    if (have < clen) return Parse::Incomplete;
    out.body = body;
    out.body_len = clen;
    return Parse::Complete;
}
const char* reason(int s) {
    switch (s) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Error";
    }
}
size_t build_response(char* out, size_t cap, int status, const char* body, const char* extra) {
    unsigned blen = (unsigned)std::strlen(body);
    int n = std::snprintf(out, cap,
        "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nX-Pip-Protocol: 0\r\n%s%sConnection: close\r\n\r\n%s",
        status, reason(status), blen, extra ? extra : "", extra ? "\r\n" : "", body);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
}
```

- [ ] **Step 4: Run host tests** Expected: `3/3 tests passed`.
- [ ] **Step 5: Commit** `feat(core): HTTP/1.0 request parser and JSON response builder`

### Task 4: Protocol routing and event state machines (core)

**Files:**
- Create: `core/include/pip/protocol.hpp`, `core/src/protocol.cpp`, `core/include/pip/events.hpp`, `core/src/events.cpp`, `tests/test_protocol.cpp`, `tests/test_events.cpp`
- Modify: `tests/CMakeLists.txt` (two `pip_test` lines)

**Interfaces:**
- Produces: `pip::Emotion {Idle,Happy,Sleepy,Thinking,Alert,Wink,Count}`, `pip::Chirp {Rise,Trill,Drop,Purr,Count}`, name/from functions, `pip::Senses {float light_lux; float temp_c; bool button_down;}`, `pip::Body` interface (`express(Emotion)`, `chirp(Chirp)`, `led(r,g,b)`, `senses()`), `pip::handle_request(req, body, out, cap) -> bytes`, `pip::event_json(name,out,cap)`, `pip::Event {None,ButtonPress,ButtonHold,ButtonRelease,LightLow,LightHigh}`, `event_name`, `pip::ButtonFsm::tick(now_ms,pressed)->Event`, `pip::LightFsm::tick(now_ms,lux)->Event`.

- [ ] **Step 1: Failing tests**

`tests/test_protocol.cpp`:
```cpp
#include <cstring>
#include <string>
#include "check.h"
#include "pip/protocol.hpp"
#include "pip/http.hpp"
using namespace pip;
struct Fake : Body {
    Emotion e = Emotion::Idle; Chirp c = Chirp::Rise; int r = -1, g = -1, b = -1; int n_express = 0, n_chirp = 0, n_led = 0;
    void express(Emotion x) override { e = x; ++n_express; }
    void chirp(Chirp x) override { c = x; ++n_chirp; }
    void led(uint8_t rr, uint8_t gg, uint8_t bb) override { r = rr; g = gg; b = bb; ++n_led; }
    Senses senses() override { return Senses{123.4f, 25.5f, true}; }
};
static std::string call(Fake& f, const char* raw) {
    http::Request req{}; CHECK(http::parse_request(raw, std::strlen(raw), req) == http::Parse::Complete);
    char out[512]; size_t n = handle_request(req, f, out, sizeof out); CHECK(n > 0); return std::string(out, n);
}
static void run() {
    Fake f;
    std::string r = call(f, "POST /express HTTP/1.0\r\nContent-Length: 19\r\n\r\n{\"emotion\":\"happy\"}");
    CHECK(r.rfind("HTTP/1.0 200", 0) == 0); CHECK(r.find("{\"ok\":true}") != std::string::npos);
    CHECK(f.e == Emotion::Happy); CHECK_EQ(f.n_express, 1);
    r = call(f, "POST /express HTTP/1.0\r\nContent-Length: 19\r\n\r\n{\"emotion\":\"angry\"}");
    CHECK(r.rfind("HTTP/1.0 400", 0) == 0); CHECK(r.find("{\"error\":\"unknown emotion\"}") != std::string::npos); CHECK_EQ(f.n_express, 1);
    r = call(f, "POST /express HTTP/1.0\r\nContent-Length: 2\r\n\r\n{}");
    CHECK(r.rfind("HTTP/1.0 400", 0) == 0); CHECK(r.find("missing emotion") != std::string::npos);
    r = call(f, "POST /chirp HTTP/1.0\r\nContent-Length: 16\r\n\r\n{\"name\":\"trill\"}");
    CHECK(r.rfind("HTTP/1.0 200", 0) == 0); CHECK(f.c == Chirp::Trill);
    r = call(f, "POST /led HTTP/1.0\r\nContent-Length: 22\r\n\r\n{\"r\":255,\"g\":0,\"b\":16}");
    CHECK(r.rfind("HTTP/1.0 200", 0) == 0); CHECK_EQ(f.r, 255); CHECK_EQ(f.g, 0); CHECK_EQ(f.b, 16);
    r = call(f, "POST /led HTTP/1.0\r\nContent-Length: 22\r\n\r\n{\"r\":256,\"g\":0,\"b\":16}");
    CHECK(r.rfind("HTTP/1.0 400", 0) == 0); CHECK_EQ(f.n_led, 1);
    r = call(f, "GET /senses HTTP/1.0\r\n\r\n");
    CHECK(r.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(r.find("{\"light_lux\":123.4,\"temp_c\":25.5,\"button\":\"down\"}") != std::string::npos);
    CHECK(r.find("X-Pip-Protocol: 0\r\n") != std::string::npos);
    r = call(f, "GET /express HTTP/1.0\r\n\r\n");
    CHECK(r.rfind("HTTP/1.0 405", 0) == 0);
    r = call(f, "GET /nope HTTP/1.0\r\n\r\n");
    CHECK(r.rfind("HTTP/1.0 404", 0) == 0);
    char ev[64]; size_t n = event_json("button.press", ev, sizeof ev); ev[n] = 0;
    CHECK_STREQ(ev, "{\"event\":\"button.press\"}");
    CHECK_STREQ(emotion_name(Emotion::Wink), "wink"); CHECK_STREQ(chirp_name(Chirp::Purr), "purr");
    Emotion e; CHECK(emotion_from("sleepy", e)); CHECK(e == Emotion::Sleepy); CHECK(!emotion_from("", e));
}
TEST_MAIN()
```

`tests/test_events.cpp`:
```cpp
#include <cstring>
#include "check.h"
#include "pip/events.hpp"
using namespace pip;
static void run() {
    ButtonFsm b(1500, 30);
    CHECK(b.tick(0, false) == Event::None);
    CHECK(b.tick(10, true) == Event::None);         // bounce window
    CHECK(b.tick(20, false) == Event::None);        // bounced back
    CHECK(b.tick(30, true) == Event::None);
    CHECK(b.tick(70, true) == Event::ButtonPress);  // stable 40ms
    CHECK(b.down());
    CHECK(b.tick(1000, true) == Event::None);
    CHECK(b.tick(1600, true) == Event::ButtonHold); // 1530ms after the accepted press at 70
    CHECK(b.tick(1700, true) == Event::None);       // hold fires once
    CHECK(b.tick(1710, false) == Event::None);
    CHECK(b.tick(1750, false) == Event::ButtonRelease);
    CHECK(!b.down());
    // short press never holds
    CHECK(b.tick(2000, true) == Event::None); CHECK(b.tick(2040, true) == Event::ButtonPress);
    CHECK(b.tick(2100, false) == Event::None); CHECK(b.tick(2140, false) == Event::ButtonRelease);
    CHECK_STREQ(event_name(Event::ButtonHold), "button.hold");
    CHECK_STREQ(event_name(Event::LightLow), "light.low");

    LightFsm l(10.0f, 20.0f, 30000);
    CHECK(l.tick(0, 100.0f) == Event::None);
    CHECK(l.tick(1000, 5.0f) == Event::None);        // starts timing
    CHECK(l.tick(20000, 5.0f) == Event::None);
    CHECK(l.tick(25000, 50.0f) == Event::None);      // interrupted, timer resets
    CHECK(l.tick(26000, 5.0f) == Event::None);
    CHECK(l.tick(55000, 5.0f) == Event::None);       // 29s, not yet
    CHECK(l.tick(56000, 5.0f) == Event::LightLow);   // 30s sustained
    CHECK(l.tick(57000, 5.0f) == Event::None);       // fires once
    CHECK(l.tick(58000, 15.0f) == Event::None);      // between thresholds: hysteresis holds
    CHECK(l.tick(59000, 25.0f) == Event::LightHigh);
    CHECK(l.tick(60000, 25.0f) == Event::None);
}
TEST_MAIN()
```

- [ ] **Step 2: Run, expect compile failure**

- [ ] **Step 3: Implement**

`core/include/pip/protocol.hpp`:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include "pip/http.hpp"
namespace pip {
enum class Emotion : uint8_t { Idle, Happy, Sleepy, Thinking, Alert, Wink, Count };
enum class Chirp : uint8_t { Rise, Trill, Drop, Purr, Count };
const char* emotion_name(Emotion e);
bool emotion_from(const char* s, Emotion& out);
const char* chirp_name(Chirp c);
bool chirp_from(const char* s, Chirp& out);
struct Senses { float light_lux; float temp_c; bool button_down; };
// What the protocol drives. Implementations must be cheap and safe to call from an lwIP
// callback: set a pending value, let the main loop act on it.
struct Body {
    virtual ~Body() = default;
    virtual void express(Emotion e) = 0;
    virtual void chirp(Chirp c) = 0;
    virtual void led(uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual Senses senses() = 0;
};
// Routes one parsed request per PROTOCOL.md v0 and writes the full HTTP response. Returns bytes.
size_t handle_request(const http::Request& req, Body& body, char* out, size_t cap);
// {"event":"<name>"} for POST <brain>/event. Returns bytes.
size_t event_json(const char* event, char* out, size_t cap);
}
```

`core/src/protocol.cpp`:
```cpp
#include "pip/protocol.hpp"
#include <cstdio>
#include <cstring>
#include "pip/json_mini.hpp"
namespace pip {
namespace {
const char* const kEmotions[] = {"idle", "happy", "sleepy", "thinking", "alert", "wink"};
const char* const kChirps[] = {"rise", "trill", "drop", "purr"};
template <size_t N>
bool lookup(const char* const (&names)[N], const char* s, uint8_t& idx) {
    for (size_t i = 0; i < N; ++i) if (std::strcmp(names[i], s) == 0) { idx = (uint8_t)i; return true; }
    return false;
}
size_t respond(char* out, size_t cap, int status, const char* body) { return http::build_response(out, cap, status, body, nullptr); }
}
const char* emotion_name(Emotion e) { return kEmotions[(uint8_t)e]; }
bool emotion_from(const char* s, Emotion& out) { uint8_t i; if (!lookup(kEmotions, s, i)) return false; out = (Emotion)i; return true; }
const char* chirp_name(Chirp c) { return kChirps[(uint8_t)c]; }
bool chirp_from(const char* s, Chirp& out) { uint8_t i; if (!lookup(kChirps, s, i)) return false; out = (Chirp)i; return true; }

size_t handle_request(const http::Request& req, Body& body, char* out, size_t cap) {
    bool is_post = std::strcmp(req.method, "POST") == 0;
    bool is_get = std::strcmp(req.method, "GET") == 0;
    if (std::strcmp(req.path, "/express") == 0) {
        if (!is_post) return respond(out, cap, 405, "{\"error\":\"use POST\"}");
        char name[16];
        if (!json::get_string(req.body, req.body_len, "emotion", name, sizeof name)) return respond(out, cap, 400, "{\"error\":\"missing emotion\"}");
        Emotion e;
        if (!emotion_from(name, e)) return respond(out, cap, 400, "{\"error\":\"unknown emotion\"}");
        body.express(e);
        return respond(out, cap, 200, "{\"ok\":true}");
    }
    if (std::strcmp(req.path, "/chirp") == 0) {
        if (!is_post) return respond(out, cap, 405, "{\"error\":\"use POST\"}");
        char name[16];
        if (!json::get_string(req.body, req.body_len, "name", name, sizeof name)) return respond(out, cap, 400, "{\"error\":\"missing name\"}");
        Chirp c;
        if (!chirp_from(name, c)) return respond(out, cap, 400, "{\"error\":\"unknown chirp\"}");
        body.chirp(c);
        return respond(out, cap, 200, "{\"ok\":true}");
    }
    if (std::strcmp(req.path, "/led") == 0) {
        if (!is_post) return respond(out, cap, 405, "{\"error\":\"use POST\"}");
        long r, g, b;
        if (!json::get_int(req.body, req.body_len, "r", &r) || !json::get_int(req.body, req.body_len, "g", &g) || !json::get_int(req.body, req.body_len, "b", &b))
            return respond(out, cap, 400, "{\"error\":\"need r,g,b\"}");
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return respond(out, cap, 400, "{\"error\":\"r,g,b must be 0-255\"}");
        body.led((uint8_t)r, (uint8_t)g, (uint8_t)b);
        return respond(out, cap, 200, "{\"ok\":true}");
    }
    if (std::strcmp(req.path, "/senses") == 0) {
        if (!is_get) return respond(out, cap, 405, "{\"error\":\"use GET\"}");
        Senses s = body.senses();
        char js[96];
        std::snprintf(js, sizeof js, "{\"light_lux\":%.1f,\"temp_c\":%.1f,\"button\":\"%s\"}", (double)s.light_lux, (double)s.temp_c, s.button_down ? "down" : "up");
        return respond(out, cap, 200, js);
    }
    return respond(out, cap, 404, "{\"error\":\"not found\"}");
}
size_t event_json(const char* event, char* out, size_t cap) {
    int n = std::snprintf(out, cap, "{\"event\":\"%s\"}", event);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
}
```

`core/include/pip/events.hpp`:
```cpp
#pragma once
#include <cstdint>
namespace pip {
enum class Event : uint8_t { None, ButtonPress, ButtonHold, ButtonRelease, LightLow, LightHigh };
const char* event_name(Event e);   // "button.press" etc., nullptr for None
// Debounced button with a one-shot hold. Feed it the raw pin state every few ms.
class ButtonFsm {
public:
    explicit ButtonFsm(uint32_t hold_ms = 1500, uint32_t debounce_ms = 30) : hold_ms_(hold_ms), debounce_ms_(debounce_ms) {}
    Event tick(uint32_t now_ms, bool pressed);
    bool down() const { return stable_; }
private:
    uint32_t hold_ms_, debounce_ms_;
    bool raw_ = false, stable_ = false, hold_sent_ = false;
    uint32_t raw_since_ = 0, press_at_ = 0;
};
// light.low once lux stays under low_lux for sustain_ms; light.high once it rises over high_lux.
class LightFsm {
public:
    LightFsm(float low_lux = 10.0f, float high_lux = 20.0f, uint32_t sustain_ms = 30000) : low_(low_lux), high_(high_lux), sustain_(sustain_ms) {}
    Event tick(uint32_t now_ms, float lux);
    bool is_low() const { return low_state_; }
private:
    float low_, high_;
    uint32_t sustain_;
    bool low_state_ = false, timing_ = false;
    uint32_t below_since_ = 0;
};
}
```

`core/src/events.cpp`:
```cpp
#include "pip/events.hpp"
namespace pip {
const char* event_name(Event e) {
    switch (e) {
        case Event::ButtonPress: return "button.press";
        case Event::ButtonHold: return "button.hold";
        case Event::ButtonRelease: return "button.release";
        case Event::LightLow: return "light.low";
        case Event::LightHigh: return "light.high";
        default: return nullptr;
    }
}
Event ButtonFsm::tick(uint32_t now, bool pressed) {
    if (pressed != raw_) { raw_ = pressed; raw_since_ = now; }
    if (raw_ != stable_ && now - raw_since_ >= debounce_ms_) {
        stable_ = raw_;
        if (stable_) { press_at_ = now; hold_sent_ = false; return Event::ButtonPress; }
        return Event::ButtonRelease;
    }
    if (stable_ && !hold_sent_ && now - press_at_ >= hold_ms_) { hold_sent_ = true; return Event::ButtonHold; }
    return Event::None;
}
Event LightFsm::tick(uint32_t now, float lux) {
    if (!low_state_) {
        if (lux < low_) {
            if (!timing_) { timing_ = true; below_since_ = now; }
            else if (now - below_since_ >= sustain_) { low_state_ = true; timing_ = false; return Event::LightLow; }
        } else {
            timing_ = false;
        }
    } else if (lux > high_) {
        low_state_ = false;
        return Event::LightHigh;
    }
    return Event::None;
}
}
```

- [ ] **Step 4: Run host tests** Expected: `5/5 tests passed`, zero failed checks.
- [ ] **Step 5: Commit** `feat(core): protocol v0 routing, Body interface, button and light state machines`

### Task 5: Framebuffer and face engine (core)

**Files:**
- Create: `core/include/pip/face.hpp`, `core/src/face.cpp`, `tests/test_face.cpp`
- Modify: `tests/CMakeLists.txt` (`pip_test(test_face)`)

**Interfaces:**
- Produces: `pip::Rect`, `pip::Framebuffer {W=320,H=240, px[], fill, fill_rect, fill_ellipse, at}`, `pip::rgb565`, `pip::EyeShape`, `pip::FaceShape`, `pip::shape_for(Emotion)`, `pip::Face {set_emotion, emotion, tick(dt_ms, fb)->Rect dirty, current()}`. Device code pushes `dirty` rows from `fb.px` to the panel.

- [ ] **Step 1: Failing tests**

`tests/test_face.cpp`:
```cpp
#include <cstdio>
#include <cstring>
#include "check.h"
#include "pip/face.hpp"
using namespace pip;
static Framebuffer fb;   // 150 KB, static on purpose
static void dump_ppm(const char* path) {
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", Framebuffer::W, Framebuffer::H);
    for (int i = 0; i < Framebuffer::W * Framebuffer::H; ++i) {
        uint16_t p = fb.px[i];
        unsigned char rgb[3] = {(unsigned char)((p >> 8) & 0xF8), (unsigned char)((p >> 3) & 0xFC), (unsigned char)((p << 3) & 0xF8)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}
static void settle(Face& f, int frames = 40) { for (int i = 0; i < frames; ++i) f.tick(16, fb); }
static void run() {
    CHECK_EQ(rgb565(255, 255, 255), (uint16_t)0xFFFF); CHECK_EQ(rgb565(255, 0, 0), (uint16_t)0xF800);
    fb.fill(0); fb.fill_ellipse(100, 100, 20, 10, 0xFFFF);
    CHECK_EQ(fb.at(100, 100), (uint16_t)0xFFFF); CHECK_EQ(fb.at(119, 100), (uint16_t)0xFFFF);
    CHECK_EQ(fb.at(100, 109), (uint16_t)0xFFFF); CHECK_EQ(fb.at(121, 100), (uint16_t)0);
    CHECK_EQ(fb.at(118, 109), (uint16_t)0);   // outside the ellipse corner
    fb.fill_rect(Rect{-5, -5, 10, 10}, 0x1234); CHECK_EQ(fb.at(0, 0), (uint16_t)0x1234); CHECK_EQ(fb.at(5, 5), (uint16_t)0);  // clipped

    Face face;
    Rect d = face.tick(16, fb);
    CHECK(!d.empty()); CHECK_EQ(d.x, (int16_t)0); CHECK_EQ(d.w, (int16_t)Framebuffer::W);   // first frame paints everything
    settle(face);
    CHECK(face.emotion() == Emotion::Idle);
    CHECK_EQ(fb.at(Face::LEFT_CX, Face::EYE_CY), Face::PUPIL);   // pupil centred at idle
    CHECK_EQ(fb.at(Face::LEFT_CX - 30, Face::EYE_CY), Face::EYE);
    CHECK_EQ(fb.at(10, 10), Face::BG);
    d = face.tick(16, fb); CHECK(d.empty());   // settled, no blink yet (first blink after 2.8 s)
    dump_ppm("face_idle.ppm");

    face.set_emotion(Emotion::Wink); settle(face);
    CHECK(face.current().left.lid_top_pct == 100); CHECK(face.current().right.lid_top_pct == 0);
    CHECK_EQ(fb.at(Face::LEFT_CX, Face::EYE_CY), Face::BG);      // left eye shut
    CHECK_EQ(fb.at(Face::RIGHT_CX, Face::EYE_CY), Face::PUPIL);  // right open
    dump_ppm("face_wink.ppm");

    face.set_emotion(Emotion::Happy); d = face.tick(16, fb); CHECK(!d.empty());   // animating
    settle(face);
    CHECK(face.current().left.ry == shape_for(Emotion::Happy).left.ry);
    CHECK(face.current().left.lid_bottom_pct == shape_for(Emotion::Happy).left.lid_bottom_pct);
    dump_ppm("face_happy.ppm");

    face.set_emotion(Emotion::Sleepy); settle(face); dump_ppm("face_sleepy.ppm");
    face.set_emotion(Emotion::Thinking); settle(face);
    CHECK(face.current().left.pupil_dx == shape_for(Emotion::Thinking).left.pupil_dx);
    dump_ppm("face_thinking.ppm");
    face.set_emotion(Emotion::Alert); settle(face); dump_ppm("face_alert.ppm");

    // Blink: at idle, advance past the first blink time and catch lids shut, then open again.
    Face f2; f2.tick(16, fb); settle(f2);
    bool saw_shut = false, saw_open_after = false;
    for (int t = 0; t < 4000; t += 16) {
        f2.tick(16, fb);
        uint16_t c = fb.at(Face::LEFT_CX, Face::EYE_CY);
        if (c == Face::BG) saw_shut = true; else if (saw_shut) saw_open_after = true;
    }
    CHECK(saw_shut); CHECK(saw_open_after);
}
TEST_MAIN()
```

- [ ] **Step 2: Run, expect compile failure**

- [ ] **Step 3: Implement**

`core/include/pip/face.hpp`:
```cpp
#pragma once
#include <cstdint>
#include "pip/protocol.hpp"
namespace pip {
struct Rect { int16_t x, y, w, h; bool empty() const { return w <= 0 || h <= 0; } };
Rect rect_union(Rect a, Rect b);
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// RGB565 landscape framebuffer, 320x240, 150 KB. Lives in .bss on the Pico 2 W (520 KB SRAM).
struct Framebuffer {
    static constexpr int W = 320, H = 240;
    uint16_t px[W * H];
    void fill(uint16_t c);
    void fill_rect(Rect r, uint16_t c);                       // clipped
    void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t c);   // filled, clipped
    uint16_t at(int x, int y) const { return px[y * W + x]; }
};
struct EyeShape {
    int16_t rx, ry;             // half-size
    int16_t lid_top_pct;        // 0 open .. 100 shut, from the top
    int16_t lid_bottom_pct;     // 0 .. 100, from the bottom (happy squint)
    int16_t pupil_dx, pupil_dy; // pupil offset from the eye centre
    int16_t pupil_r;
};
struct FaceShape { EyeShape left, right; };
FaceShape shape_for(Emotion e);
// Eyes-first face. Call tick() every frame; it animates toward the target emotion, blinks
// on its own, redraws only what changed, and returns the dirty rectangle to push.
class Face {
public:
    static constexpr int LEFT_CX = 110, RIGHT_CX = 210, EYE_CY = 120;
    static constexpr uint16_t BG = rgb565(12, 12, 28), EYE = rgb565(240, 240, 255), PUPIL = rgb565(20, 20, 40);
    Face();
    void set_emotion(Emotion e);
    Emotion emotion() const { return target_emotion_; }
    Rect tick(uint32_t dt_ms, Framebuffer& fb);
    const FaceShape& current() const { return cur_; }
private:
    static void step(int16_t& v, int16_t target, uint32_t dt_ms);
    static Rect eye_bounds(int cx, const EyeShape& e);
    static void draw_eye(Framebuffer& fb, int cx, const EyeShape& e);
    Emotion target_emotion_ = Emotion::Idle;
    FaceShape cur_, target_, drawn_;
    bool first_ = true, blinking_ = false;
    uint32_t t_ms_ = 0, next_blink_ms_ = 2800, blink_until_ms_ = 0;
    unsigned blink_n_ = 0;
};
}
```

`core/src/face.cpp`:
```cpp
#include "pip/face.hpp"
#include <algorithm>
#include <cstring>
namespace pip {
Rect rect_union(Rect a, Rect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    int16_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    int16_t x1 = std::max((int16_t)(a.x + a.w), (int16_t)(b.x + b.w)), y1 = std::max((int16_t)(a.y + a.h), (int16_t)(b.y + b.h));
    return Rect{x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}
void Framebuffer::fill(uint16_t c) { for (int i = 0; i < W * H; ++i) px[i] = c; }
void Framebuffer::fill_rect(Rect r, uint16_t c) {
    int x0 = std::max(0, (int)r.x), y0 = std::max(0, (int)r.y);
    int x1 = std::min(W, (int)r.x + (int)r.w), y1 = std::min(H, (int)r.y + (int)r.h);
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) px[y * W + x] = c;
}
void Framebuffer::fill_ellipse(int cx, int cy, int rx, int ry, uint16_t c) {
    if (rx <= 0 || ry <= 0) return;
    for (int dy = -ry; dy <= ry; ++dy) {
        int y = cy + dy; if (y < 0 || y >= H) continue;
        // half-width at this row: rx * sqrt(1 - (dy/ry)^2), in integer math
        long num = (long)rx * rx * ((long)ry * ry - (long)dy * dy);
        long den = (long)ry * ry;
        int hw = 0; long q = num / den; while ((long)(hw + 1) * (hw + 1) <= q) ++hw;
        int x0 = std::max(0, cx - hw), x1 = std::min(W - 1, cx + hw);
        for (int x = x0; x <= x1; ++x) px[y * W + x] = c;
    }
}
FaceShape shape_for(Emotion e) {
    EyeShape base{40, 50, 0, 0, 0, 0, 14};
    FaceShape s{base, base};
    switch (e) {
        case Emotion::Idle: break;
        case Emotion::Happy: s.left.ry = s.right.ry = 36; s.left.lid_bottom_pct = s.right.lid_bottom_pct = 45; break;
        case Emotion::Sleepy: s.left.lid_top_pct = s.right.lid_top_pct = 60; s.left.pupil_dy = s.right.pupil_dy = 8; break;
        case Emotion::Thinking: s.left.pupil_dx = s.right.pupil_dx = 14; s.left.pupil_dy = s.right.pupil_dy = -16; s.left.lid_top_pct = s.right.lid_top_pct = 15; break;
        case Emotion::Alert: s.left.rx = s.right.rx = 44; s.left.ry = s.right.ry = 58; s.left.pupil_r = s.right.pupil_r = 10; break;
        case Emotion::Wink: s.left.lid_top_pct = 100; break;
        default: break;
    }
    return s;
}
Face::Face() : cur_(shape_for(Emotion::Idle)), target_(cur_), drawn_(cur_) {}
void Face::set_emotion(Emotion e) { target_emotion_ = e; target_ = shape_for(e); }
void Face::step(int16_t& v, int16_t target, uint32_t dt_ms) {
    int diff = target - v;
    if (diff == 0) return;
    int s = (diff * (int)dt_ms) / 120;          // ~120 ms to traverse, ease-out
    if (s == 0) s = diff > 0 ? 1 : -1;
    v = (int16_t)(v + s);
    if ((diff > 0 && v > target) || (diff < 0 && v < target)) v = target;
}
Rect Face::eye_bounds(int cx, const EyeShape& e) {
    return Rect{(int16_t)(cx - e.rx - 2), (int16_t)(EYE_CY - e.ry - 2), (int16_t)(2 * e.rx + 5), (int16_t)(2 * e.ry + 5)};
}
void Face::draw_eye(Framebuffer& fb, int cx, const EyeShape& e) {
    fb.fill_ellipse(cx, EYE_CY, e.rx, e.ry, EYE);
    fb.fill_ellipse(cx + e.pupil_dx, EYE_CY + e.pupil_dy, e.pupil_r, e.pupil_r, PUPIL);
    int top = (2 * e.ry * e.lid_top_pct) / 100, bottom = (2 * e.ry * e.lid_bottom_pct) / 100;
    if (top > 0) fb.fill_rect(Rect{(int16_t)(cx - e.rx - 1), (int16_t)(EYE_CY - e.ry - 1), (int16_t)(2 * e.rx + 3), (int16_t)(top + 1)}, BG);
    if (bottom > 0) fb.fill_rect(Rect{(int16_t)(cx - e.rx - 1), (int16_t)(EYE_CY + e.ry - bottom), (int16_t)(2 * e.rx + 3), (int16_t)(bottom + 2)}, BG);
}
Rect Face::tick(uint32_t dt_ms, Framebuffer& fb) {
    t_ms_ += dt_ms;
    auto both = [&](auto fn) { fn(cur_.left, target_.left); fn(cur_.right, target_.right); };
    both([&](EyeShape& c, const EyeShape& t) {
        step(c.rx, t.rx, dt_ms); step(c.ry, t.ry, dt_ms);
        step(c.lid_top_pct, t.lid_top_pct, dt_ms); step(c.lid_bottom_pct, t.lid_bottom_pct, dt_ms);
        step(c.pupil_dx, t.pupil_dx, dt_ms); step(c.pupil_dy, t.pupil_dy, dt_ms); step(c.pupil_r, t.pupil_r, dt_ms);
    });
    // Blink schedule: pseudo-random gaps from a counter so tests are deterministic.
    if (!blinking_ && t_ms_ >= next_blink_ms_) { blinking_ = true; blink_until_ms_ = t_ms_ + 120; }
    if (blinking_ && t_ms_ >= blink_until_ms_) { blinking_ = false; ++blink_n_; next_blink_ms_ = t_ms_ + 2800 + (blink_n_ * 577u) % 1900u; }
    FaceShape show = cur_;
    if (blinking_) { show.left.lid_top_pct = 100; if (target_emotion_ != Emotion::Wink) show.right.lid_top_pct = 100; }
    bool changed = first_ || std::memcmp(&show, &drawn_, sizeof show) != 0;
    if (!changed) return Rect{0, 0, 0, 0};
    Rect dirty;
    if (first_) {
        fb.fill(BG);
        dirty = Rect{0, 0, Framebuffer::W, Framebuffer::H};
        first_ = false;
    } else {
        dirty = rect_union(rect_union(eye_bounds(LEFT_CX, drawn_.left), eye_bounds(LEFT_CX, show.left)),
                           rect_union(eye_bounds(RIGHT_CX, drawn_.right), eye_bounds(RIGHT_CX, show.right)));
        fb.fill_rect(dirty, BG);
    }
    draw_eye(fb, LEFT_CX, show.left);
    draw_eye(fb, RIGHT_CX, show.right);
    drawn_ = show;
    return dirty;
}
}
```

- [ ] **Step 4: Run host tests; look at the PPMs**

Run the suite: expected `6/6 tests passed`. Then convert and view: `cd build-host && for f in face_*.ppm; do sips -s format png "$f" --out "${f%.ppm}.png" >/dev/null; done; ls *.png` and Read `face_idle.png`, `face_happy.png`, `face_wink.png`. Expected: two large pale eyes on a dark ground, happy shows a squint from below, wink has the left eye shut. If an expression reads wrong (pupil outside the eye, lid gap), fix the numbers in `shape_for` and rerun; the tests constrain geometry only loosely on purpose.

- [ ] **Step 5: Commit** `feat(core): framebuffer primitives and the eyes-first face engine` (do not commit the PPM/PNG files; add `tests/*.ppm` and `build-host/` are already ignored by `build-*/`).

### Task 6: ILI9341 driver and the face on the panel (device)

**Files:**
- Create: `firmware/src/drivers/ili9341.hpp`, `firmware/src/drivers/ili9341.cpp`
- Modify: `firmware/CMakeLists.txt` (add the driver source when not PIP_LITE), `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `pip::Framebuffer`, `pip::Face`, `pip::Rect` (Task 5), `pip::pins` (Task 1).
- Produces: `pip::drv::Ili9341 {init(spi, baud), push(fb, rect)}`.

- [ ] **Step 1: Driver**

`firmware/src/drivers/ili9341.hpp`:
```cpp
#pragma once
#include "hardware/spi.h"
#include "pip/face.hpp"
namespace pip::drv {
// ILI9341 over SPI0, landscape 320x240, RGB565. Commands are 8-bit, pixels 16-bit.
class Ili9341 {
public:
    void init(spi_inst_t* spi, unsigned baud_hz);
    void push(const Framebuffer& fb, Rect r);   // pushes the rows of r from fb, blocking
private:
    void cmd(uint8_t c);
    void data(const uint8_t* d, size_t n);
    void window(int x0, int y0, int x1, int y1);
    spi_inst_t* spi_ = nullptr;
};
}
```

`firmware/src/drivers/ili9341.cpp`:
```cpp
#include "drivers/ili9341.hpp"
#include <algorithm>
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pins.hpp"
namespace pip::drv {
namespace {
struct Step { uint8_t cmd; uint8_t n; uint8_t d[15]; uint16_t delay_ms; };
// Standard ILI9341 bring-up (same sequence the Adafruit driver uses). MADCTL 0x28 = MV|BGR, landscape.
const Step kInit[] = {
    {0xEF, 3, {0x03, 0x80, 0x02}, 0}, {0xCF, 3, {0x00, 0xC1, 0x30}, 0}, {0xED, 4, {0x64, 0x03, 0x12, 0x81}, 0},
    {0xE8, 3, {0x85, 0x00, 0x78}, 0}, {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}, 0}, {0xF7, 1, {0x20}, 0},
    {0xEA, 2, {0x00, 0x00}, 0}, {0xC0, 1, {0x23}, 0}, {0xC1, 1, {0x10}, 0}, {0xC5, 2, {0x3E, 0x28}, 0},
    {0xC7, 1, {0x86}, 0}, {0x36, 1, {0x28}, 0}, {0x37, 1, {0x00}, 0}, {0x3A, 1, {0x55}, 0},
    {0xB1, 2, {0x00, 0x18}, 0}, {0xB6, 3, {0x08, 0x82, 0x27}, 0}, {0xF2, 1, {0x00}, 0}, {0x26, 1, {0x01}, 0},
    {0xE0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 0},
    {0xE1, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 0},
    {0x11, 0, {}, 120}, {0x29, 0, {}, 20},
};
}
void Ili9341::init(spi_inst_t* spi, unsigned baud_hz) {
    spi_ = spi;
    spi_init(spi_, baud_hz);
    spi_set_format(spi_, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(pins::SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(pins::SPI_MOSI, GPIO_FUNC_SPI);
    for (unsigned p : {pins::LCD_CS, pins::LCD_DC, pins::LCD_RST}) { gpio_init(p); gpio_set_dir(p, true); gpio_put(p, 1); }
    gpio_put(pins::LCD_RST, 0); sleep_ms(10); gpio_put(pins::LCD_RST, 1); sleep_ms(120);
    for (const Step& s : kInit) { cmd(s.cmd); if (s.n) data(s.d, s.n); if (s.delay_ms) sleep_ms(s.delay_ms); }
}
void Ili9341::cmd(uint8_t c) { gpio_put(pins::LCD_DC, 0); gpio_put(pins::LCD_CS, 0); spi_write_blocking(spi_, &c, 1); gpio_put(pins::LCD_CS, 1); }
void Ili9341::data(const uint8_t* d, size_t n) { gpio_put(pins::LCD_DC, 1); gpio_put(pins::LCD_CS, 0); spi_write_blocking(spi_, d, n); gpio_put(pins::LCD_CS, 1); }
void Ili9341::window(int x0, int y0, int x1, int y1) {
    uint8_t c[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t p[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    cmd(0x2A); data(c, 4); cmd(0x2B); data(p, 4); cmd(0x2C);
}
void Ili9341::push(const Framebuffer& fb, Rect r) {
    int x0 = std::max(0, (int)r.x), y0 = std::max(0, (int)r.y);
    int x1 = std::min(Framebuffer::W, (int)r.x + (int)r.w), y1 = std::min(Framebuffer::H, (int)r.y + (int)r.h);
    if (x1 <= x0 || y1 <= y0) return;
    window(x0, y0, x1 - 1, y1 - 1);
    gpio_put(pins::LCD_DC, 1); gpio_put(pins::LCD_CS, 0);
    spi_set_format(spi_, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    for (int y = y0; y < y1; ++y) spi_write16_blocking(spi_, &fb.px[y * Framebuffer::W + x0], (size_t)(x1 - x0));
    spi_set_format(spi_, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(pins::LCD_CS, 1);
}
}
```
In `firmware/CMakeLists.txt`, inside the `else()` (not PIP_LITE) branch add `target_sources(pip PRIVATE src/drivers/ili9341.cpp)`; add `target_include_directories(pip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)` next to the existing include line.

- [ ] **Step 2: main.cpp runs the face at 30 fps**

Replace `firmware/src/main.cpp`:
```cpp
#include <cstdio>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pins.hpp"
#include "pip/face.hpp"
#ifndef PIP_LITE
#include "drivers/ili9341.hpp"
#endif

static pip::Framebuffer g_fb;

int main() {
    stdio_init_all();
    if (cyw43_arch_init() != 0) { printf("pip: cyw43 init failed\n"); return 1; }
    pip::Face face;
#ifndef PIP_LITE
    pip::drv::Ili9341 lcd;
    lcd.init(spi0, 40 * 1000 * 1000);
#endif
    // Demo loop until the protocol lands: cycle expressions every 3 s so the bench has something to look at.
    const pip::Emotion cycle[] = {pip::Emotion::Idle, pip::Emotion::Happy, pip::Emotion::Thinking, pip::Emotion::Wink, pip::Emotion::Sleepy, pip::Emotion::Alert};
    unsigned ci = 0;
    absolute_time_t next = get_absolute_time(), next_cycle = make_timeout_time_ms(3000);
    uint32_t last_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        uint32_t dt = now_ms - last_ms; last_ms = now_ms;
        if (absolute_time_diff_us(get_absolute_time(), next_cycle) <= 0) {
            ci = (ci + 1) % 6; face.set_emotion(cycle[ci]);
            printf("pip: express %s\n", pip::emotion_name(cycle[ci]));
            next_cycle = make_timeout_time_ms(3000);
        }
        pip::Rect dirty = face.tick(dt, g_fb);
#ifndef PIP_LITE
        if (!dirty.empty()) lcd.push(g_fb, dirty);
#else
        (void)dirty;
#endif
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / 500) % 2);
        next = delayed_by_ms(next, 33);
        sleep_until(next);
    }
}
```

- [ ] **Step 3: Build both variants, flash the full one**

Run both builds (Global Constraints). Expected: both link. Then `picotool load -f -x build-fw/pip.uf2`; read serial 7 s: expect `pip: express happy` then `pip: express thinking`.

- [ ] **Step 4: Bench (Riadh)**

Report `DONE_WITH_CONCERNS: bench unverified` with this checklist for Riadh: panel shows two pale eyes on a near-black ground, expression changes every 3 s, eyes blink on their own. If the panel stays white: backlight is on but no init reached it, check DC/CS/RST wiring and that VCC is on 3V3 (or 5V if the board has a regulator). If colours look inverted or the image is mirrored, try MADCTL `0xE8` instead of `0x28` in `kInit` (and report which worked so the README states it).

- [ ] **Step 5: Commit** `feat(firmware): ILI9341 driver, face on the panel at 30 fps`

### Task 7: Senses and actuators: button, RGB LED, VEML7700, temp; event detection (device)

**Files:**
- Create: `firmware/src/drivers/button.hpp`, `firmware/src/drivers/rgb_led.hpp`, `firmware/src/drivers/veml7700.hpp`, `firmware/src/drivers/veml7700.cpp`, `firmware/src/drivers/temp.hpp`
- Modify: `firmware/CMakeLists.txt`, `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `pip::ButtonFsm`, `pip::LightFsm`, `pip::Event`, `event_name` (Task 4).
- Produces: `pip::drv::button_init()/button_pressed()`, `pip::drv::RgbLed {init(common_anode), set(r,g,b)}`, `pip::drv::Veml7700 {init(i2c) -> bool, read_lux(float&) -> bool}`, `pip::drv::temp_init()/temp_read_c()`.

- [ ] **Step 1: Drivers (header-only where trivial)**

`firmware/src/drivers/button.hpp`:
```cpp
#pragma once
#include "hardware/gpio.h"
#include "pins.hpp"
namespace pip::drv {
inline void button_init() { gpio_init(pins::BUTTON); gpio_set_dir(pins::BUTTON, false); gpio_pull_up(pins::BUTTON); }
inline bool button_pressed() { return !gpio_get(pins::BUTTON); }   // wired to GND, active low
}
```

`firmware/src/drivers/rgb_led.hpp`:
```cpp
#pragma once
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pins.hpp"
namespace pip::drv {
class RgbLed {
public:
    void init(bool common_anode) {
        anode_ = common_anode;
        for (unsigned p : {pins::LED_R, pins::LED_G, pins::LED_B}) {
            gpio_set_function(p, GPIO_FUNC_PWM);
            unsigned slice = pwm_gpio_to_slice_num(p);
            pwm_set_wrap(slice, 255);
            pwm_set_enabled(slice, true);
        }
        set(0, 0, 0);
    }
    void set(uint8_t r, uint8_t g, uint8_t b) {
        pwm_set_gpio_level(pins::LED_R, anode_ ? 255 - r : r);
        pwm_set_gpio_level(pins::LED_G, anode_ ? 255 - g : g);
        pwm_set_gpio_level(pins::LED_B, anode_ ? 255 - b : b);
    }
private:
    bool anode_ = false;
};
}
```

`firmware/src/drivers/temp.hpp`:
```cpp
#pragma once
#include "hardware/adc.h"
namespace pip::drv {
inline void temp_init() { adc_init(); adc_set_temp_sensor_enabled(true); }
inline float temp_read_c() {
    adc_select_input(4);
    float v = adc_read() * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;   // RP2350 datasheet formula
}
}
```

`firmware/src/drivers/veml7700.hpp`:
```cpp
#pragma once
#include "hardware/i2c.h"
namespace pip::drv {
// VEML7700 ambient light sensor, gain 1x, 100 ms integration (0.0576 lux/count).
class Veml7700 {
public:
    bool init(i2c_inst_t* i2c);      // false when the sensor does not ACK
    bool read_lux(float& lux);
private:
    i2c_inst_t* i2c_ = nullptr;
};
}
```
`firmware/src/drivers/veml7700.cpp`:
```cpp
#include "drivers/veml7700.hpp"
#include "hardware/gpio.h"
#include "pins.hpp"
namespace pip::drv {
namespace { constexpr uint8_t ADDR = 0x10, REG_CONF = 0x00, REG_ALS = 0x04; }
bool Veml7700::init(i2c_inst_t* i2c) {
    i2c_ = i2c;
    i2c_init(i2c_, 100 * 1000);
    gpio_set_function(pins::I2C_SDA, GPIO_FUNC_I2C); gpio_set_function(pins::I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(pins::I2C_SDA); gpio_pull_up(pins::I2C_SCL);
    uint8_t conf[3] = {REG_CONF, 0x00, 0x00};   // gain 1x, IT 100 ms, power on
    return i2c_write_blocking(i2c_, ADDR, conf, 3, false) == 3;
}
bool Veml7700::read_lux(float& lux) {
    uint8_t reg = REG_ALS, buf[2];
    if (i2c_write_blocking(i2c_, ADDR, &reg, 1, true) != 1) return false;
    if (i2c_read_blocking(i2c_, ADDR, buf, 2, false) != 2) return false;
    lux = (float)(buf[0] | (buf[1] << 8)) * 0.0576f;
    return true;
}
}
```
CMake: add `src/drivers/veml7700.cpp` to the non-lite sources.

- [ ] **Step 2: main.cpp gains senses, FSMs, and event logging**

Edit `firmware/src/main.cpp`: add includes `"pip/events.hpp"`, `"drivers/button.hpp"`, `"drivers/temp.hpp"`, and under `#ifndef PIP_LITE` `"drivers/rgb_led.hpp"`, `"drivers/veml7700.hpp"`. After `cyw43_arch_init` add:
```cpp
    pip::drv::button_init();
    pip::drv::temp_init();
    pip::ButtonFsm btn;
    pip::LightFsm light;
    float lux = -1.0f, temp_c = 0.0f;
#ifndef PIP_LITE
    pip::drv::RgbLed rgb; rgb.init(false);   // PIP_RGB_COMMON_ANODE: flip to true if the LED reads inverted
    pip::drv::Veml7700 als; bool have_als = als.init(i2c0);
    printf("pip: veml7700 %s\n", have_als ? "ok" : "absent");
#endif
    uint32_t next_sense_ms = 0;
```
Inside the loop, before `face.tick`:
```cpp
        pip::Event ev = btn.tick(now_ms, pip::drv::button_pressed());
        if (now_ms >= next_sense_ms) {
            next_sense_ms = now_ms + 500;
            temp_c = pip::drv::temp_read_c();
#ifndef PIP_LITE
            if (have_als && als.read_lux(lux)) {
                pip::Event lev = light.tick(now_ms, lux);
                if (lev != pip::Event::None) ev = lev;   // at most one event per frame is fine at 30 fps
            }
#endif
        }
        if (ev != pip::Event::None) {
            printf("pip: event %s (lux=%.1f temp=%.1f)\n", pip::event_name(ev), (double)lux, (double)temp_c);
#ifndef PIP_LITE
            if (ev == pip::Event::ButtonPress) rgb.set(0, 40, 0);
            if (ev == pip::Event::ButtonRelease) rgb.set(0, 0, 0);
#endif
        }
```
Keep the expression demo cycle for now.

- [ ] **Step 3: Build both, flash, verify on serial**

Flash `build-fw/pip.uf2`, read serial for 10 s while pressing the button once briefly and once for 2 s. Expected lines: `pip: veml7700 ok` (or `absent` if not wired yet, which is acceptable and must be reported), `pip: event button.press`, `button.release`, then `button.press`, `button.hold`, `button.release`, with plausible `temp=` values (20 to 35). Report `DONE_WITH_CONCERNS: bench unverified` only for the LED colour (green while held) and the lux number if the sensor is wired.

- [ ] **Step 4: Commit** `feat(firmware): button, RGB LED, VEML7700, temp, and event detection on the body`

### Task 8: WiFi and the HTTP tool server (device)

**Files:**
- Create: `firmware/config.example.h`, `firmware/src/net/wifi.hpp`, `firmware/src/net/wifi.cpp`, `firmware/src/net/http_server.hpp`, `firmware/src/net/http_server.cpp`, `firmware/src/body.hpp`
- Modify: `firmware/CMakeLists.txt`, `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `pip::Body`, `pip::handle_request`, `pip::http::*` (Tasks 3-4), drivers (Tasks 6-7).
- Produces: `pip::net::wifi_connect(ssid,pass,timeout_ms,char ip[16]) -> bool`, `pip::net::http_server_start(port, Body&) -> bool`, `pip::RealBody` (pending-value mailbox the main loop drains).

- [ ] **Step 1: Config**

`firmware/config.example.h`:
```cpp
#pragma once
// Copy to config.h (git-ignored) and fill in. Never commit config.h.
#define PIP_WIFI_SSID "your-ssid"
#define PIP_WIFI_PASS "your-password"
#define PIP_BRAIN_HOST "192.168.1.50"   // dotted IPv4 of the brain (Pi Zero 2 W); DNS is out of scope for v0
#define PIP_BRAIN_PORT 8080
#define PIP_HTTP_PORT 80
#define PIP_RGB_COMMON_ANODE 0
```
In `firmware/CMakeLists.txt` after `project(...)`:
```cmake
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config.h)
  message(FATAL_ERROR "firmware/config.h missing: cp firmware/config.example.h firmware/config.h and fill in WiFi + brain")
endif()
```
Create the real `firmware/config.h` on this machine by copying the example and filling SSID/password from 1Password (`op` CLI, item for the home WiFi; if no item exists, leave placeholders, note it in the report, and Riadh fills it) and the brain host as `192.168.1.50` placeholder. Confirm `git status` never shows `config.h`.

- [ ] **Step 2: WiFi**

`firmware/src/net/wifi.hpp`:
```cpp
#pragma once
#include <cstdint>
namespace pip::net {
// Station mode, WPA2. Writes the dotted IP into ip_out on success.
bool wifi_connect(const char* ssid, const char* pass, uint32_t timeout_ms, char ip_out[16]);
}
```
`firmware/src/net/wifi.cpp`:
```cpp
#include "net/wifi.hpp"
#include <cstdio>
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
namespace pip::net {
bool wifi_connect(const char* ssid, const char* pass, uint32_t timeout_ms, char ip_out[16]) {
    cyw43_arch_enable_sta_mode();
    int rc = cyw43_arch_wifi_connect_timeout_ms(ssid, pass, CYW43_AUTH_WPA2_AES_PSK, timeout_ms);
    if (rc != 0) { printf("pip: wifi connect failed rc=%d\n", rc); return false; }
    ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_out, 16);
    return true;
}
}
```

- [ ] **Step 3: RealBody mailbox**

`firmware/src/body.hpp`:
```cpp
#pragma once
#include <atomic>
#include "pip/protocol.hpp"
namespace pip {
// Protocol handlers run inside lwIP callbacks; they only drop values here. The main loop
// drains them once per frame. Senses are published by the main loop and read here.
class RealBody : public Body {
public:
    void express(Emotion e) override { pending_emotion_.store((int)e); }
    void chirp(Chirp c) override { pending_chirp_.store((int)c); }
    void led(uint8_t r, uint8_t g, uint8_t b) override { pending_led_.store(0x01000000u | (r << 16) | (g << 8) | b); }
    Senses senses() override { return Senses{lux_.load(), temp_.load(), button_.load()}; }
    void publish(float lux, float temp_c, bool button) { lux_.store(lux); temp_.store(temp_c); button_.store(button); }
    bool take_emotion(Emotion& e) { int v = pending_emotion_.exchange(-1); if (v < 0) return false; e = (Emotion)v; return true; }
    bool take_chirp(Chirp& c) { int v = pending_chirp_.exchange(-1); if (v < 0) return false; c = (Chirp)v; return true; }
    bool take_led(uint8_t& r, uint8_t& g, uint8_t& b) { uint32_t v = pending_led_.exchange(0); if (!(v & 0x01000000u)) return false; r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF; return true; }
private:
    std::atomic<int> pending_emotion_{-1}, pending_chirp_{-1};
    std::atomic<uint32_t> pending_led_{0};
    std::atomic<float> lux_{-1.0f}, temp_{0.0f};
    std::atomic<bool> button_{false};
};
}
```

- [ ] **Step 4: HTTP server over raw lwIP tcp**

`firmware/src/net/http_server.hpp`:
```cpp
#pragma once
#include <cstdint>
#include "pip/protocol.hpp"
namespace pip::net {
// Raw-tcp HTTP/1.0 server for PROTOCOL.md v0. One request per connection. Four concurrent
// connections; extra ones are closed. Call once from main after WiFi is up.
bool http_server_start(uint16_t port, Body& body);
}
```
`firmware/src/net/http_server.cpp`:
```cpp
#include "net/http_server.hpp"
#include <cstdio>
#include <cstring>
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "pip/http.hpp"
namespace pip::net {
namespace {
constexpr int kConns = 4;
struct Conn { tcp_pcb* pcb = nullptr; char buf[1536]; size_t len = 0; bool used = false; };
Conn g_conns[kConns];
Body* g_body = nullptr;

void conn_close(Conn* c) {
    if (c->pcb) {
        tcp_arg(c->pcb, nullptr); tcp_recv(c->pcb, nullptr); tcp_err(c->pcb, nullptr); tcp_sent(c->pcb, nullptr);
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
    }
    c->pcb = nullptr; c->len = 0; c->used = false;
}
void on_err(void* arg, err_t) { Conn* c = static_cast<Conn*>(arg); if (c) { c->pcb = nullptr; conn_close(c); } }
err_t on_recv(void* arg, tcp_pcb* pcb, pbuf* p, err_t) {
    Conn* c = static_cast<Conn*>(arg);
    if (!p) { conn_close(c); return ERR_OK; }
    size_t room = sizeof c->buf - 1 - c->len;
    size_t take = p->tot_len < room ? p->tot_len : room;
    pbuf_copy_partial(p, c->buf + c->len, (u16_t)take, 0);
    c->len += take;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    http::Request req{};
    http::Parse st = http::parse_request(c->buf, c->len, req);
    if (st == http::Parse::Incomplete && take == room) st = http::Parse::Bad;   // buffer full, give up
    if (st == http::Parse::Incomplete) return ERR_OK;
    static char resp[512];
    size_t n = (st == http::Parse::Bad) ? http::build_response(resp, sizeof resp, 400, "{\"error\":\"bad request\"}", nullptr)
                                        : handle_request(req, *g_body, resp, sizeof resp);
    if (n == 0) n = http::build_response(resp, sizeof resp, 400, "{\"error\":\"response too large\"}", nullptr);
    tcp_write(pcb, resp, (u16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    conn_close(c);   // close after write: lwIP flushes queued data before FIN
    return ERR_OK;
}
err_t on_accept(void*, tcp_pcb* newpcb, err_t err) {
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    Conn* c = nullptr;
    for (Conn& k : g_conns) if (!k.used) { c = &k; break; }
    if (!c) { tcp_abort(newpcb); return ERR_ABRT; }
    c->used = true; c->pcb = newpcb; c->len = 0;
    tcp_arg(newpcb, c); tcp_recv(newpcb, on_recv); tcp_err(newpcb, on_err);
    return ERR_OK;
}
}
bool http_server_start(uint16_t port, Body& body) {
    g_body = &body;
    tcp_pcb* pcb = tcp_new();
    if (!pcb) return false;
    if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { tcp_abort(pcb); return false; }
    tcp_pcb* lpcb = tcp_listen_with_backlog(pcb, 4);
    if (!lpcb) { tcp_abort(pcb); return false; }
    tcp_accept(lpcb, on_accept);
    printf("pip: http server on :%u\n", port);
    return true;
}
}
```
CMake: add `src/net/wifi.cpp src/net/http_server.cpp` to the `pip` sources (both variants).

- [ ] **Step 5: main.cpp: connect, serve, drain the mailbox, drop the demo cycle**

In `main.cpp`: include `"config.h"`, `"body.hpp"`, `"net/wifi.hpp"`, `"net/http_server.hpp"`. After the drivers init:
```cpp
    char ip[16] = "0.0.0.0";
    bool online = pip::net::wifi_connect(PIP_WIFI_SSID, PIP_WIFI_PASS, 20000, ip);
    printf("pip: wifi %s ip=%s\n", online ? "up" : "down", ip);
    static pip::RealBody body;
    if (online) { cyw43_arch_lwip_begin(); pip::net::http_server_start(PIP_HTTP_PORT, body); cyw43_arch_lwip_end(); }
```
Remove the `cycle[]` demo block. Each frame, after the sensing block:
```cpp
        body.publish(lux, temp_c, btn.down());
        pip::Emotion pe; if (body.take_emotion(pe)) { face.set_emotion(pe); printf("pip: express %s\n", pip::emotion_name(pe)); }
        pip::Chirp pc; if (body.take_chirp(pc)) printf("pip: chirp %s (audio lands with Plan 3b)\n", pip::chirp_name(pc));
        uint8_t r, g, b; if (body.take_led(r, g, b)) {
#ifndef PIP_LITE
            rgb.set(r, g, b);
#else
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (r | g | b) != 0);
#endif
            printf("pip: led %u %u %u\n", r, g, b);
        }
```
On PIP_LITE the heartbeat LED line from Task 6 must go (the LED now belongs to /led); keep it on the full build. Use `rgb.init(PIP_RGB_COMMON_ANODE)`.

- [ ] **Step 6: Build, flash, verify with curl from the Mac**

Flash; read serial until `pip: wifi up ip=<IP>` and `pip: http server on :80`. Then from the Mac (replace IP):
```bash
curl -s -i http://<IP>/senses
curl -s -i -X POST http://<IP>/express -d '{"emotion":"happy"}'
curl -s -i -X POST http://<IP>/express -d '{"emotion":"nope"}'
curl -s -i -X POST http://<IP>/led -d '{"r":0,"g":0,"b":60}'
curl -s -i -X POST http://<IP>/chirp -d '{"name":"trill"}'
curl -s -i http://<IP>/nothing
for i in 1 2 3 4 5 6; do curl -s -o /dev/null -w '%{http_code} ' http://<IP>/senses; done; echo
```
Expected: 200 with `{"light_lux":...,"temp_c":...,"button":"up"}` and header `X-Pip-Protocol: 0`; 200 `{"ok":true}`; 400 `{"error":"unknown emotion"}`; 200; 200; 404; six `200`s (connections recycle). Serial shows `pip: express happy`, `pip: led 0 0 60`, `pip: chirp trill`. Bench concern to report: face turned happy and LED went blue.

- [ ] **Step 7: Commit** `feat(firmware): WiFi, raw-tcp HTTP tool server, RealBody mailbox, config.example.h`

### Task 9: Event POST to the brain (device)

**Files:**
- Create: `firmware/src/net/event_post.hpp`, `firmware/src/net/event_post.cpp`
- Modify: `firmware/CMakeLists.txt`, `firmware/src/main.cpp`

**Interfaces:**
- Consumes: `pip::event_json`, `pip::event_name` (Task 4).
- Produces: `pip::net::post_event(host_ip, port, json) -> bool` (at-most-once, fire and forget; false when a previous post is still in flight or the address is invalid). Call it only between `cyw43_arch_lwip_begin()`/`end()`.

- [ ] **Step 1: Implement**

`firmware/src/net/event_post.hpp`:
```cpp
#pragma once
#include <cstdint>
namespace pip::net {
// One-shot "POST /event" with a JSON body, no retry, at most one in flight. Caller holds the lwIP lock.
bool post_event(const char* host_ip, uint16_t port, const char* json);
}
```
`firmware/src/net/event_post.cpp`:
```cpp
#include "net/event_post.hpp"
#include <cstdio>
#include <cstring>
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
namespace pip::net {
namespace {
struct Post { tcp_pcb* pcb = nullptr; char req[320]; size_t len = 0; bool busy = false; };
Post g_post;
void finish() {
    if (g_post.pcb) {
        tcp_arg(g_post.pcb, nullptr); tcp_recv(g_post.pcb, nullptr); tcp_err(g_post.pcb, nullptr);
        if (tcp_close(g_post.pcb) != ERR_OK) tcp_abort(g_post.pcb);
    }
    g_post.pcb = nullptr; g_post.busy = false;
}
void on_err(void*, err_t e) { printf("pip: event post err %d\n", (int)e); g_post.pcb = nullptr; finish(); }
err_t on_recv(void*, tcp_pcb* pcb, pbuf* p, err_t) {
    if (!p) { finish(); return ERR_OK; }
    tcp_recved(pcb, p->tot_len); pbuf_free(p);   // the brain's reply is not interesting; close once it arrives
    finish();
    return ERR_OK;
}
err_t on_connected(void*, tcp_pcb* pcb, err_t err) {
    if (err != ERR_OK) { finish(); return ERR_OK; }
    tcp_write(pcb, g_post.req, (u16_t)g_post.len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}
}
bool post_event(const char* host_ip, uint16_t port, const char* json) {
    if (g_post.busy) { printf("pip: event dropped, post in flight\n"); return false; }
    ip_addr_t addr;
    if (!ipaddr_aton(host_ip, &addr)) return false;
    int n = std::snprintf(g_post.req, sizeof g_post.req,
        "POST /event HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
        host_ip, (unsigned)std::strlen(json), json);
    if (n < 0 || (size_t)n >= sizeof g_post.req) return false;
    g_post.len = (size_t)n;
    g_post.pcb = tcp_new();
    if (!g_post.pcb) return false;
    g_post.busy = true;
    tcp_arg(g_post.pcb, &g_post); tcp_err(g_post.pcb, on_err); tcp_recv(g_post.pcb, on_recv);
    if (tcp_connect(g_post.pcb, &addr, port, on_connected) != ERR_OK) { finish(); return false; }
    return true;
}
}
```
CMake: add `src/net/event_post.cpp`. In `main.cpp` include `"net/event_post.hpp"`; in the `if (ev != pip::Event::None)` block, after the printf:
```cpp
            if (online) {
                char js[64]; pip::event_json(pip::event_name(ev), js, sizeof js);
                cyw43_arch_lwip_begin();
                pip::net::post_event(PIP_BRAIN_HOST, PIP_BRAIN_PORT, js);
                cyw43_arch_lwip_end();
            }
```

- [ ] **Step 2: Verify against a fake brain on the Mac**

Set `PIP_BRAIN_HOST` in `config.h` to the Mac's LAN IP (`ipconfig getifaddr en0`) and `PIP_BRAIN_PORT 8080`; rebuild and flash. On the Mac run `python3 -m http.server 8080 --bind 0.0.0.0` (it logs every request and answers POST with 501, which is fine for v0) in the background to a log file. Press the button short, then long. Expected in the python log: `"POST /event HTTP/1.0" 501` three times for press/release and five for press/hold/release; serial shows the matching `pip: event ...` lines and no `dropped` lines. Stop the server. Report any `event post err` codes.

- [ ] **Step 3: Commit** `feat(firmware): at-most-once event POST to the brain`

### Task 10: I2S smoke on RP2350 (go/no-go for audio)

**Files:**
- Create: `firmware/smoke/i2s_tone.cpp`, `firmware/pico_extras_import.cmake` (copy of `~/git/pico-extras/external/pico_extras_import.cmake`)
- Modify: `firmware/CMakeLists.txt`

**Interfaces:**
- Produces: a written answer in the report and README: does `pico_audio_i2s` drive the MAX98357A on RP2350 with the default pins. Plan 3b (chirp synth) is designed on that answer.

- [ ] **Step 1: Target**

Append to `firmware/CMakeLists.txt`:
```cmake
if(PIP_AUDIO_SMOKE)
  include(pico_extras_import.cmake)
  add_executable(pip_i2s_smoke smoke/i2s_tone.cpp)
  target_compile_definitions(pip_i2s_smoke PRIVATE
    PICO_AUDIO_I2S_DATA_PIN=28 PICO_AUDIO_I2S_CLOCK_PIN_BASE=26)
  target_link_libraries(pip_i2s_smoke pico_stdlib pico_audio_i2s)
  pico_enable_stdio_usb(pip_i2s_smoke 1)
  pico_enable_stdio_uart(pip_i2s_smoke 0)
  pico_add_extra_outputs(pip_i2s_smoke)
endif()
```
The `include(pico_extras_import.cmake)` must come before `project()` to take effect the way `pico_sdk_import` does; put it right after `include(pico_sdk_import.cmake)` guarded by `if(PIP_AUDIO_SMOKE)`, and keep only the target block at the bottom.

`firmware/smoke/i2s_tone.cpp`:
```cpp
#include <cmath>
#include <cstdio>
#include "pico/stdlib.h"
#include "pico/audio_i2s.h"

// Two seconds of 440 Hz, then two of silence, forever. Stereo S16 at 44.1 kHz, the same
// shape as pico-playground's sine_wave example. If this sings on RP2350, Plan 3b is a go.
int main() {
    stdio_init_all();
    static audio_format_t fmt = {44100, AUDIO_BUFFER_FORMAT_PCM_S16, 2};
    static audio_buffer_format_t bfmt = {&fmt, 4};
    audio_buffer_pool_t* pool = audio_new_producer_pool(&bfmt, 3, 256);
    audio_i2s_config_t cfg = {PICO_AUDIO_I2S_DATA_PIN, PICO_AUDIO_I2S_CLOCK_PIN_BASE, 0, 0};
    const audio_format_t* out = audio_i2s_setup(&fmt, &cfg);
    if (!out) { printf("i2s: setup failed\n"); return 1; }
    if (!audio_i2s_connect(pool)) { printf("i2s: connect failed\n"); return 1; }
    audio_i2s_set_enabled(true);
    printf("i2s: running, 440 Hz bursts on DIN=%d BCLK=%d LRCLK=%d\n", PICO_AUDIO_I2S_DATA_PIN, PICO_AUDIO_I2S_CLOCK_PIN_BASE, PICO_AUDIO_I2S_CLOCK_PIN_BASE + 1);
    uint32_t phase = 0, n = 0;
    const uint32_t step = (uint32_t)(440.0 * 4294967296.0 / 44100.0);
    while (true) {
        audio_buffer_t* b = take_audio_buffer(pool, true);
        int16_t* s = (int16_t*)b->buffer->bytes;
        bool on = ((n / 44100) % 4) < 2;
        for (uint i = 0; i < b->max_sample_count; ++i, ++n) {
            phase += step;
            int16_t v = on ? (int16_t)(std::sin(phase * (6.283185307 / 4294967296.0)) * 6000) : 0;
            s[2 * i] = v; s[2 * i + 1] = v;
        }
        b->sample_count = b->max_sample_count;
        give_audio_buffer(pool, b);
    }
}
```

- [ ] **Step 2: Build and flash**

`cmake -S firmware -B build-fw-audio -G Ninja -DPIP_AUDIO_SMOKE=ON && cmake --build build-fw-audio --target pip_i2s_smoke`. If pico-extras fails to compile for RP2350, capture the first error verbatim: that is the answer ("no-go as-is, error X"), report it, skip flashing. If it builds: `picotool load -f -x build-fw-audio/pip_i2s_smoke.uf2`, read serial, expect `i2s: running`.

- [ ] **Step 3: Bench (Riadh)**

Wiring: MAX98357A VIN to 3V3 (or VSYS), GND, BCLK GP26, LRC GP27, DIN GP28, speaker on the + and - terminals. Report `DONE_WITH_CONCERNS: bench unverified`: Riadh listens for a 2 s tone every 4 s. Either answer goes verbatim into the README's "Audio" line.

- [ ] **Step 4: Commit** `chore(firmware): I2S tone smoke for the RP2350 audio go/no-go`

### Task 11: README: wiring, build, flash, curl cookbook, status

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Rewrite the README sections**

Replace the `Status:` line and everything from `## Hardware` down with (keep the opening paragraph):
```markdown
Status: body firmware v0 on `main`. Face, senses, HTTP tool server, and event
POSTs work on a Pico 2 W; audio is the next plan (see the I2S line below).
The brain (tiny_agent + rete_cpp on a Pi Zero 2 W) lands after it. Design
spec: [tiny_agent](https://github.com/tinyagent-cc/tiny_agent)
`docs/superpowers/specs/2026-08-22-pip-companion-design.md`.

## Hardware and wiring

| Part | Role | Pico 2 W pins |
|---|---|---|
| ILI9341 240x320 SPI | the face | SCK GP18, MOSI GP19, CS GP17, DC GP20, RST GP21, LED 3V3, VCC 3V3, GND |
| VEML7700 | ambient light | SDA GP4, SCL GP5, VIN 3V3, GND |
| Tactile button | interaction | GP15 to GND (internal pull-up) |
| RGB LED (4-pin) | mood | R GP10, G GP11, B GP12 through 220 ohm, common to GND |
| MAX98357A + speaker | chirps (next plan) | BCLK GP26, LRC GP27, DIN GP28, VIN 3V3, GND |

`pins.hpp` in `firmware/` is the source of truth for this table.

`pip-lite`: `-DPIP_LITE=ON` builds the same firmware for a bare Pico 2 W:
onboard LED answers `/led`, expressions and chirps print to serial, light
reads as -1.

## Build and flash

    export PICO_SDK_PATH=~/git/pico-sdk PICO_TOOLCHAIN_PATH=<arm-gnu-toolchain>/bin
    cp firmware/config.example.h firmware/config.h   # WiFi + brain address, never committed
    cmake -S firmware -B build-fw -G Ninja && cmake --build build-fw
    picotool load -x build-fw/pip.uf2                # board in BOOTSEL
    picotool load -f -x build-fw/pip.uf2             # board already running Pip

Serial is USB CDC (`/dev/cu.usbmodem*`, 115200). Host-side tests for the
platform-free core (`core/`): `cmake -S tests -B build-host && cmake --build
build-host && ctest --test-dir build-host`.

## Talk to the body

    curl -s http://<pip-ip>/senses
    curl -s -X POST http://<pip-ip>/express -d '{"emotion":"happy"}'
    curl -s -X POST http://<pip-ip>/chirp   -d '{"name":"trill"}'
    curl -s -X POST http://<pip-ip>/led     -d '{"r":0,"g":0,"b":60}'

Events (`button.press`, `button.hold`, `button.release`, `light.low`,
`light.high`) are POSTed to `http://<brain>/event` as `{"event":"..."}`,
at most once each. The contract is `PROTOCOL.md`.

## Audio on RP2350

<one sentence with the Task 10 result, verbatim from the bench: tone heard with pico_audio_i2s defaults, or the first error>

## Layout

    core/       platform-free C++17: face engine, HTTP + JSON, protocol, event FSMs (host-tested)
    firmware/   Pico SDK glue: drivers, WiFi, raw-tcp HTTP server, event POST, main loop
    tests/      host test harness for core/
```
Sweep the prose against `ai-tells.md` and `tells.md`. Zero em dashes.

- [ ] **Step 2: Commit** `docs: wiring, build, flash, curl cookbook, audio status`

### Task 12: Merge and push

- [ ] **Step 1:** Host tests green, both firmware variants build, `git status` shows no `config.h`.
- [ ] **Step 2:** `git checkout main && git merge --no-ff feat/firmware-v0 -m "feat: Pip body firmware v0 ... <trailer>" && git push origin main && git branch -d feat/firmware-v0`.
- [ ] **Step 3:** `gh run watch` the host-tests workflow; expected green.

## Self-review notes

Spec coverage (Track 1 body): face engine with the six expressions and region redraw (T5, T6); tool server with the four endpoints (T3, T4, T8); event push on button and light bands (T4, T7, T9); config.h pattern (T8); pip-lite (T1, T6-T8 gates); host-side tests for face and protocol (T2-T5); I2S go/no-go before audio (T10). Not in this plan by design: chirp engine and audio output (Plan 3b after T10's answer), touch, TTS, the 30 fps golden-image compare (replaced by pixel assertions plus PPM dumps a human looks at), DMA pixel push (blocking 16-bit writes are well under budget at 40 MHz).

Types: `Rect`/`Framebuffer`/`Face` names match between T5 and T6; `Body`/`Senses`/`handle_request` between T4 and T8; `Event`/`event_name`/`event_json` between T4, T7, T9; `pins::*` between T1 and every driver. Placeholders: none; the README's audio line is explicitly filled from T10's bench result.
