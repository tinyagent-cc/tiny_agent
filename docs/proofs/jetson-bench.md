# Jetson Orin Nano benchmark run, 2026-08-21

Raw output behind the Jetson section of [../benchmarks.md](../benchmarks.md). Every number there comes from a block below. Nothing is reconstructed after the fact.

The board had never seen tiny_agent before this run, and JetPack had shipped without the CUDA toolkit, so the log starts from an empty `~/tools`.

## Device and toolchain

```
$ uname -a
Linux orin-desktop 5.15.185-tegra #1 SMP PREEMPT Thu Jan 15 19:24:38 PST 2026 aarch64 aarch64 aarch64 GNU/Linux

$ cat /proc/device-tree/model
NVIDIA Jetson Orin Nano Engineering Reference Developer Kit Super

$ cat /etc/nv_tegra_release
# R36 (release), REVISION: 5.0, GCID: 43688277, BOARD: generic, EABI: aarch64, DATE: Fri Jan 16 03:50:45 UTC 2026

$ lsb_release -ds
Ubuntu 22.04.5 LTS

$ lscpu
Architecture:                            aarch64
CPU(s):                                  6
On-line CPU(s) list:                     0-5
Model name:                              Cortex-A78AE
CPU max MHz:                             1728,0000

$ free -h
               total        used        free      shared  buff/cache   available
Mem:           7,4Gi       4,1Gi       593Mi       5,0Mi       2,7Gi       3,0Gi

$ nvpmodel -q
NV Power Mode: MAXN_SUPER
2

$ g++ --version
g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0
```

The board arrived with `g++`, `git` and `make`, and without `cmake`, `curl`, `podman`, `docker`, or any CUDA toolkit. The GPU driver was present (`/usr/lib/aarch64-linux-gnu/tegra/libcuda.so.1`) but `nvcc`, `cudart` and cuBLAS were not, so no CUDA program could be compiled as shipped.

## What had to be installed

`curl` and the CUDA toolkit needed root. Everything else went under `~/tools`, mirroring the Pi 5 setup.

```
$ sudo apt-get install -y curl
$ sudo apt-get install -y cuda-toolkit-12-6      # from repo.download.nvidia.com/jetson/common r36.5

$ /usr/local/cuda/bin/nvcc --version
Cuda compilation tools, release 12.6, V12.6.68
Build cuda_12.6.r12.6/compiler.34714021_0

$ cd ~/tools && curl -sL -o cmake.tar.gz \
    https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-aarch64.tar.gz
$ tar xzf cmake.tar.gz && ~/tools/cmake-3.31.6-linux-aarch64/bin/cmake --version
cmake version 3.31.6

$ git clone --depth 1 https://github.com/microsoft/vcpkg.git ~/tools/vcpkg
$ ~/tools/vcpkg/bootstrap-vcpkg.sh -disableMetrics
vcpkg package management program version 2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8

$ git clone https://github.com/rhajamor/tiny_agent.git ~/tiny_agent_cpp
$ git -C ~/tiny_agent_cpp rev-parse HEAD
4c8eeea65595ef913a591d070689bf8af35f44f2
```

## Configure

`cmake --preset release` with `VCPKG_ROOT=~/tools/vcpkg`. This is a first-time configure, so it builds nlohmann-json, cpp-httplib with OpenSSL, doctest, libenvpp and json-schema-validator from source. OpenSSL 3.6.3 is most of the 245 s.

```
  # this is heuristically generated, and may not be correct
  find_package(libenvpp CONFIG REQUIRED)
  target_link_libraries(main PRIVATE libenvpp::libenvpp)

Completed submission of fmt:arm64-linux@12.2.0#1 to 1 binary cache(s) in 160 ms
Waiting for 1 remaining binary cache submissions...
Completed submission of libenvpp:arm64-linux@1.5.3 to 1 binary cache(s) in 272 ms (1/1)
All requested installations completed successfully in: 4 min
-- Running vcpkg install - done
-- The CXX compiler identification is GNU 11.4.0
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /usr/bin/c++ - skipped
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- Found nlohmann_json: /home/orin/tiny_agent_cpp/build/vcpkg_installed/arm64-linux/share/nlohmann_json/nlohmann_jsonConfig.cmake (found version "3.12.0")
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success
-- Found Threads: TRUE
-- Found OpenSSL: /home/orin/tiny_agent_cpp/build/vcpkg_installed/arm64-linux/lib/libcrypto.a (found suitable version "3.6.3", minimum required is "3.0.0") found components: Crypto SSL
-- Found httplib: /home/orin/tiny_agent_cpp/build/vcpkg_installed/arm64-linux/include/httplib.h (found version "0.53.0")
-- Configuring done (244.9s)
-- Generating done (0.1s)
-- Build files have been written to: /home/orin/tiny_agent_cpp/build
CONFIGURE_SECONDS 245 rc=0
```

## Build

Clean rebuild with all vcpkg dependencies already in place, `-j6`, `make -k` so one bad target does not hide the rest.

```
$ cmake --build build --target clean && cmake --build build -j6 -- -k
[ 89%] Building CXX object tests/CMakeFiles/test_readme_snippets.dir/test_readme_snippets.cpp.o
[ 90%] Linking CXX executable test_summarize
[ 90%] Built target test_summarize
[ 91%] Building CXX object tests/CMakeFiles/test_agent.dir/test_agent.cpp.o
[ 92%] Linking CXX executable test_hardening
[ 92%] Built target test_hardening
[ 93%] Building CXX object bench/CMakeFiles/bench_agent.dir/bench_agent.cpp.o
[ 94%] Linking CXX executable test_context
[ 94%] Built target test_context
[ 95%] Linking CXX executable test_vectorstore_remote
[ 96%] Linking CXX executable test_tracing
[ 96%] Built target test_vectorstore_remote
[ 96%] Built target test_tracing
[ 97%] Linking CXX executable test_readme_snippets
[ 97%] Built target test_readme_snippets
[ 98%] Linking CXX executable test_agent
[ 98%] Built target test_agent
[ 99%] Linking CXX executable bench_agent
[ 99%] Built target bench_agent
gmake: *** [Makefile:101: all] Error 2
FULL_BUILD_SECONDS 524 rc=2
```

52 of 53 targets link. The single failure is `16_deep_agent_custom`, and it is a g++ 11.4 limitation rather than anything Jetson-specific:

```
/home/orin/tiny_agent_cpp/examples/16_deep_agent_custom.cpp: In function ‘int main()’:
/home/orin/tiny_agent_cpp/examples/16_deep_agent_custom.cpp:60:35: error: call of overloaded ‘init_chat_model(const char [19], <brace-enclosed initializer list>)’ is ambiguous
   60 |             .llm = init_chat_model("openai:gpt-4o-mini", {.api_key = key}),
      |                    ~~~~~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
In file included from /home/orin/tiny_agent_cpp/include/tiny_agent/middleware/model_fallback.hpp:3,
                 from /home/orin/tiny_agent_cpp/include/tiny_agent/middleware/all.hpp:15,
                 from /home/orin/tiny_agent_cpp/include/tiny_agent/tiny_agent.hpp:11,
                 from /home/orin/tiny_agent_cpp/examples/16_deep_agent_custom.cpp:1:
/home/orin/tiny_agent_cpp/include/tiny_agent/middleware/../init_chat_model.hpp:38:16: note: candidate: ‘tiny_agent::AnyChat tiny_agent::init_chat_model(const string&, tiny_agent::LLMConfig)’
   38 | inline AnyChat init_chat_model(const std::string& model_string,
      |                ^~~~~~~~~~~~~~~
/home/orin/tiny_agent_cpp/include/tiny_agent/middleware/../init_chat_model.hpp:53:16: note: candidate: ‘tiny_agent::AnyChat tiny_agent::init_chat_model(const string&, const string&, tiny_agent::LLMConfig)’
   53 | inline AnyChat init_chat_model(const std::string& provider,
      |                ^~~~~~~~~~~~~~~
/home/orin/tiny_agent_cpp/examples/16_deep_agent_custom.cpp:59:79: error: cannot convert ‘<brace-enclosed initializer list>’ to ‘tiny_agent::create_agent<tiny_agent::agents::deep_agent_tag>::Params<tiny_agent::ChatVariant<tiny_agent::OpenAI, tiny_agent::Anthropic, tiny_agent::Gemini> >’
   59 |         auto from_init = create_agent<agents::deep_agent_tag>::create<AnyChat>({
      |                          ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~^~
   60 |             .llm = init_chat_model("openai:gpt-4o-mini", {.api_key = key}),
      |             ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    
   61 |             .name = "from_init",
      |             ~~~~~~~~~~~~~~~~~~~~                                               
   62 |             .system_prompt = "Answer concisely."
      |             ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~                               
   63 |         });
      |         ~~                                                                     
In file included from /home/orin/tiny_agent_cpp/include/tiny_agent/agent.hpp:4,
                 from /home/orin/tiny_agent_cpp/include/tiny_agent/tiny_agent.hpp:14,
                 from /home/orin/tiny_agent_cpp/examples/16_deep_agent_custom.cpp:1:
/home/orin/tiny_agent_cpp/include/tiny_agent/agents/create.hpp:26:36: note:   initializing argument 1 of ‘static auto tiny_agent::create_agent<tiny_agent::agents::deep_agent_tag>::create(tiny_agent::create_agent<tiny_agent::agents::deep_agent_tag>::Params<LLM>) [with LLM = tiny_agent::ChatVariant<tiny_agent::OpenAI, tiny_agent::Anthropic, tiny_agent::Gemini>]’
```

g++ 11 keeps both `init_chat_model` overloads in the candidate set because the second argument is a braced initializer, and then cannot choose. Clang and newer GCC discard the three-parameter candidate. Nothing else in the tree hits it.

## Test suite

```
$ ctest --output-on-failure
Test project /home/orin/tiny_agent_cpp/build
      Start  1: test_types
 1/20 Test  #1: test_types .......................   Passed    0.01 sec
      Start  2: test_tool
 2/20 Test  #2: test_tool ........................   Passed    0.00 sec
      Start  3: test_sse
 3/20 Test  #3: test_sse .........................   Passed    0.00 sec
      Start  4: test_stream
 4/20 Test  #4: test_stream ......................   Passed    0.01 sec
      Start  5: test_middleware
 5/20 Test  #5: test_middleware ..................   Passed    0.01 sec
      Start  6: test_middleware_builtins
 6/20 Test  #6: test_middleware_builtins .........   Passed    0.01 sec
      Start  7: test_init_chat_model
 7/20 Test  #7: test_init_chat_model .............   Passed    0.01 sec
      Start  8: test_init_embeddings
 8/20 Test  #8: test_init_embeddings .............   Passed    0.02 sec
      Start  9: test_vectorstore
 9/20 Test  #9: test_vectorstore .................   Passed    0.00 sec
      Start 10: test_retriever
10/20 Test #10: test_retriever ...................   Passed    0.00 sec
      Start 11: test_skills
11/20 Test #11: test_skills ......................   Passed    0.01 sec
      Start 12: test_summarize
12/20 Test #12: test_summarize ...................   Passed    0.01 sec
      Start 13: test_rationalize
13/20 Test #13: test_rationalize .................   Passed    0.00 sec
      Start 14: test_memory
14/20 Test #14: test_memory ......................   Passed    0.00 sec
      Start 15: test_hardening
15/20 Test #15: test_hardening ...................   Passed    0.01 sec
      Start 16: test_tracing
16/20 Test #16: test_tracing .....................   Passed    0.01 sec
      Start 17: test_vectorstore_remote
17/20 Test #17: test_vectorstore_remote ..........   Passed    0.01 sec
      Start 18: test_context
18/20 Test #18: test_context .....................   Passed    0.01 sec
      Start 19: test_readme_snippets
19/20 Test #19: test_readme_snippets .............   Passed    0.01 sec
      Start 20: test_agent
20/20 Test #20: test_agent .......................   Passed    0.01 sec

100% tests passed, 0 tests failed out of 20

Total Test time (real) =   0.16 sec
```

## bench_agent

```
$ ./bench/bench_agent
╔═══════════════════════════════════════════════════════════════╗
║     tiny_agent_cpp Benchmark Suite                           ║
║     Target: Constrained Environments (RPi, Jetson, Arduino)  ║
╚═══════════════════════════════════════════════════════════════╝

=== MESSAGE & JSON OVERHEAD ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
Message::user (short)                                  64 ns       68 ns       32 ns      193 ns         14.6M
Message::user (256 chars)                              96 ns       89 ns       64 ns       19 ns         11.2M
Message::system (1KB)                                 128 ns      134 ns       64 ns       99 ns          7.4M
Message::tool_result (JSON 512B)                      128 ns      130 ns       96 ns      111 ns          7.7M
copy 50-msg history                                   3.9 us      3.9 us      3.6 us      445 ns        254.8K
copy 200-msg history (w/tools)                       14.6 us     14.8 us     14.1 us      437 ns         67.6K
json::parse (sensor payload ~300B)                    8.0 us      8.1 us      7.8 us      281 ns        124.1K
json::dump (sensor payload)                           2.3 us      2.3 us      2.2 us      120 ns        431.2K
ToolSchema create (full params)                       4.5 us      4.5 us      4.4 us      124 ns        220.7K
==============================================================================================================

=== TOOL REGISTRY ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
tool_lookup (5 tools)                                  96 ns      112 ns       96 ns       17 ns          8.9M
tool_lookup (20 tools)                                128 ns      137 ns      128 ns       14 ns          7.3M
tool_lookup (50 tools)                                128 ns      126 ns       96 ns       13 ns          7.9M
tool_lookup (100 tools)                               128 ns      116 ns       96 ns       56 ns          8.6M
tool_execute (arithmetic)                             544 ns      548 ns      480 ns       66 ns          1.8M
tool_create                                           2.8 us      2.8 us      2.7 us      136 ns        362.4K
schemas() (5 tools)                                   5.0 us      5.0 us      4.9 us      121 ns        200.6K
schemas() (20 tools)                                 30.2 us     30.2 us     26.4 us      534 ns         33.1K
schemas() (50 tools)                                 75.1 us     75.2 us     65.8 us      788 ns         13.3K
tool_execute (JSON parse sensor)                      3.0 us      3.0 us      2.8 us      248 ns        336.2K
==============================================================================================================

=== MIDDLEWARE PIPELINE ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
chain_empty (baseline)                                 96 ns       89 ns       64 ns       19 ns         11.1M
chain_runtime (depth=1)                               160 ns      155 ns      128 ns       69 ns          6.4M
chain_runtime (depth=3)                               352 ns      362 ns      288 ns      577 ns          2.8M
chain_runtime (depth=5)                               704 ns      715 ns      672 ns      323 ns          1.4M
chain_runtime (depth=10)                              2.4 us      2.4 us      2.3 us      106 ns        416.1K
chain_runtime (depth=20)                              9.0 us      9.0 us      8.9 us      228 ns        110.6K
chain_static (depth=1)                                 96 ns       82 ns       32 ns       18 ns         12.2M
chain_static (depth=3)                                 96 ns       87 ns       64 ns       42 ns         11.5M
chain_static (depth=5)                                 96 ns       93 ns       64 ns       19 ns         10.7M
chain_static (depth=10)                                96 ns      110 ns       64 ns       41 ns          9.0M
==============================================================================================================

=== BUILT-IN MIDDLEWARE ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
SystemPrompt (inject)                                 288 ns      303 ns      256 ns       65 ns          3.3M
SystemPrompt (already present)                         96 ns       92 ns       64 ns       20 ns         10.8M
TrimHistory<10> (20 msgs)                             2.8 us      2.8 us      2.7 us      100 ns        361.0K
TrimHistory<10> (50 msgs)                             7.5 us      7.5 us      7.4 us      139 ns        132.8K
TrimHistory<10> (100 msgs)                           14.8 us     14.9 us     14.6 us      216 ns         67.3K
TrimHistory<10> (200 msgs)                           29.7 us     29.8 us     29.4 us      564 ns         33.6K
Logging (off)                                         5.1 us      5.1 us      5.0 us      148 ns        194.8K
Logging (debug, sink=null)                            5.5 us      5.7 us      5.4 us      6.8 us        175.9K
PII (email, no match)                                 2.1 us      2.1 us      2.0 us      134 ns        471.0K
PII (email, 2 matches)                                4.5 us      4.5 us      4.4 us      119 ns        219.8K
ModelCallLimit (under limit)                           96 ns      108 ns       64 ns       16 ns          9.2M
ToolCallLimit (under limit)                           224 ns      223 ns      192 ns       39 ns          4.5M
summarize<100,4> (20 msgs)                            6.1 us      6.2 us      6.0 us      453 ns        162.0K
summarize<100,4> (50 msgs)                           16.9 us     16.9 us     16.4 us      270 ns         59.2K
summarize<100,4> (100 msgs)                          33.2 us     33.3 us     32.7 us      421 ns         30.0K
Rationalize (with large result)                       2.1 us      2.1 us      2.0 us       89 ns        475.9K
Rationalize (no large results)                        832 ns      821 ns      768 ns       49 ns          1.2M
ContextEditing (30 msgs, trigger=50)                  2.9 us      2.9 us      2.7 us      113 ns        347.8K
==============================================================================================================

=== MEMORY STORE & CACHE ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
LRU<32> get (hit)                                      96 ns      113 ns       96 ns       67 ns          8.8M
LRU<32> get (miss)                                     96 ns       89 ns       64 ns       19 ns         11.2M
LRU<32> put (update)                                  128 ns      115 ns       96 ns       36 ns          8.7M
LRU<32> put (evict)                                    96 ns      102 ns       64 ns       15 ns          9.8M
LRU<128> get (hit)                                     96 ns      111 ns       96 ns       17 ns          8.9M
LRU<128> put (evict)                                   96 ns      102 ns       64 ns       15 ns          9.8M
LRU<512> get (hit)                                    128 ns      113 ns       96 ns       17 ns          8.8M
LRU<512> put (evict)                                   96 ns      102 ns       64 ns       74 ns          9.8M
ToolCache lookup (hit)                                1.5 us      1.5 us      1.4 us       86 ns        659.6K
ToolCache lookup (miss)                               2.3 us      2.3 us      2.2 us      128 ns        435.5K
ToolCache store                                       2.2 us      2.2 us      2.1 us      244 ns        461.0K
cached_tool (cache hit)                               1.6 us      1.6 us      1.6 us       80 ns        606.4K
uncached_tool (same work)                             480 ns      485 ns      416 ns       33 ns          2.1M
==============================================================================================================

=== AGENT LOOP (MockLLM) ===

Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
agent.run (no tools, no mw)                           1.8 us      1.8 us      1.7 us      219 ns        551.7K
agent.run (1 tool call)                               7.4 us      7.4 us      7.2 us      223 ns        135.9K
agent.run (3 parallel tool calls)                    19.3 us     19.4 us     18.7 us      337 ns         51.6K
agent.run (3 mw + 1 tool)                             4.1 us      4.1 us      4.0 us      135 ns        242.7K
agent.run (FULL: 6 mw + 3 tools + 2 calls)           10.5 us     10.5 us     10.3 us      317 ns         95.1K
agent.chat (10 turns)                                16.4 us     16.4 us     16.2 us      254 ns         60.9K
==============================================================================================================

=== CONSTRAINED ENVIRONMENT SCENARIOS ===
(Simulating Raspberry Pi / Jetson Nano workloads)


Benchmark                                             Median        Mean         Min      Stddev       Ops/sec
--------------------------------------------------------------------------------------------------------------
GPIO monitor (1 read cycle)                           8.0 us      8.0 us      7.8 us      190 ns        125.0K
Sensor aggregation (5 sensors)                       22.5 us     22.5 us     22.0 us      440 ns         44.4K
Managed chat (30 turns, trim=15)                    166.7 us    166.9 us    165.4 us      971 ns          6.0K
PII-safe agent (3 PII filters)                       25.5 us     25.6 us     25.3 us      330 ns         39.1K
EMBEDDED PROD (6 mw + 3 tools)                        9.9 us      9.9 us      9.7 us      206 ns        101.0K
==============================================================================================================

╔═══════════════════════════════════════════════════════════════╗
║  SUMMARY — Key Numbers for Constrained Environments          ║
╚═══════════════════════════════════════════════════════════════╝

  Tool lookup (20 tools)                                128 ns  (7.3M ops/s)
  Tool execute (arithmetic)                             544 ns  (1.8M ops/s)
  Middleware chain (5 deep)                             704 ns  (1.4M ops/s)
  Static chain (5 deep)                                  96 ns  (10.7M ops/s)
  LRU<128> get (cache hit)                               96 ns  (8.9M ops/s)
  Cached tool (hit)                                     1.6 us  (606.4K ops/s)
  Agent run (no tools)                                  1.8 us  (551.7K ops/s)
  Agent run (full stack)                               10.5 us  (95.1K ops/s)
  GPIO monitor cycle                                    8.0 us  (125.0K ops/s)
  Embedded production agent                             9.9 us  (101.0K ops/s)
  json::parse (sensor payload)                          8.0 us  (124.1K ops/s)

  Static vs Runtime middleware (depth=5): 7.3x speedup
  Static vs Runtime middleware (depth=10): 25.0x speedup
  Cached vs uncached tool overhead: 3.4x

```

## Binary sizes

```
$ cd ~/tiny_agent_cpp/build
17_streaming unstripped: 9166200 bytes (8,8M)
17_streaming stripped:   7919104 bytes (7,6M)
01_basic_chat: unstripped 9135952 stripped 7898624
02_tool_calling: unstripped 9172472 stripped 7923200
06_deep_agent: unstripped 9199840 stripped 7947776
bench_agent: unstripped 9464288 stripped 8123952
```

Static OpenSSL is in every one of those, which is why a hello-world agent and a full deep agent are within 50 KB of each other.

## llama.cpp with CUDA

Built after the CUDA toolkit went on. `-j4` rather than `-j6`: nvcc is memory hungry and the flash-attention template instances will push a 7.4 GB board into swap at higher parallelism.

```
$ cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=87 \
        -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF
$ cmake --build build -j4 --target llama-server llama-bench llama-cli
-- Found Git: /usr/bin/git (found version "2.34.1")
-- Found assembler: /usr/bin/cc
-- Found Threads: TRUE
-- Found OpenMP_C: -fopenmp (found version "4.5")
-- Found OpenMP_CXX: -fopenmp (found version "4.5")
-- Found OpenMP: TRUE (found version "4.5")
-- Found CUDAToolkit: /usr/local/cuda/targets/aarch64-linux/include (found version "12.6.68")
-- CUDA Toolkit found
-- The CUDA compiler identification is NVIDIA 12.6.68 with host compiler GNU 11.4.0
-- Detecting CUDA compiler ABI info
-- Detecting CUDA compiler ABI info - done
-- Check for working CUDA compiler: /usr/local/cuda/bin/nvcc - skipped
[100%] Linking CXX shared library ../../bin/libllama-cli-impl.so
[100%] Built target llama-cli-impl
[100%] Building CXX object tools/cli/CMakeFiles/llama-cli.dir/main.cpp.o
[100%] Linking CXX executable ../../bin/llama-cli
[100%] Built target llama-cli
LLAMA_BUILD_SECONDS 1681 rc=0
```

Configure plus build, 1681 s. The linked result really does use CUDA:

```
$ ldd build/bin/llama-server | grep -i cu
	libggml-cuda.so.0 => /home/orin/tools/llama.cpp/build/bin/libggml-cuda.so.0
	libcudart.so.12 => /usr/local/cuda/targets/aarch64-linux/lib/libcudart.so.12
	libcublas.so.12 => /usr/local/cuda/targets/aarch64-linux/lib/libcublas.so.12
	libcuda.so.1 => /usr/lib/aarch64-linux-gnu/nvidia/libcuda.so.1
	libcublasLt.so.12 => /usr/local/cuda/targets/aarch64-linux/lib/libcublasLt.so.12
```

## Tokens per second

Qwen2.5-3B-Instruct Q4_K_M, three repetitions per row.

All 36 layers on the GPU:

```
$ ./build/bin/llama-bench -m ~/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf -ngl 99 -p 512 -n 128 -r 3
ggml_cuda_init: found 1 CUDA devices (Total VRAM: 7607 MiB):
  Device 0: Orin, compute capability 8.7, VMM: yes, VRAM: 7607 MiB
| model                          |       size |     params | backend    | ngl |            test |                  t/s |
| ------------------------------ | ---------: | ---------: | ---------- | --: | --------------: | -------------------: |
| qwen2 3B Q4_K - Medium         |   1.79 GiB |     3.09 B | CUDA       |  99 |           pp512 |       874.77 ± 28.22 |
| qwen2 3B Q4_K - Medium         |   1.79 GiB |     3.09 B | CUDA       |  99 |           tg128 |         23.85 ± 0.01 |

build: 873e5d8 (1)
```

CPU only. `-ngl 0` is not enough for this: with a CUDA backend loaded, llama.cpp still offloads the large prompt matmuls and reports 598 t/s on pp512. `-dev none` is the honest CPU number.

```
$ ./build/bin/llama-bench -m ~/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf -dev none -p 512 -n 128 -r 3 -t 6
| model                          |       size |     params | backend    | ngl | dev          |            test |                  t/s |
| ------------------------------ | ---------: | ---------: | ---------- | --: | ------------ | --------------: | -------------------: |
| qwen2 3B Q4_K - Medium         |   1.79 GiB |     3.09 B | CUDA       |  -1 | none         |           pp512 |         40.39 ± 0.08 |
| qwen2 3B Q4_K - Medium         |   1.79 GiB |     3.09 B | CUDA       |  -1 | none         |           tg128 |         12.75 ± 0.09 |

build: 873e5d8 (1)
```

## 17_streaming end to end

`llama-server` on the GPU, `17_streaming` pointed at it through the OpenAI-compatible path.

```
$ ./build/bin/llama-server -m ~/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf -ngl 99 \
      -c 4096 -a qwen2.5-3b-instruct --host 127.0.0.1 --port 8080
$ curl -s http://127.0.0.1:8080/v1/models
{"models":[{"name":"qwen2.5-3b-instruct","model":"qwen2.5-3b-instruct",...

$ OLLAMA_BASE_URL=http://127.0.0.1:8080 OLLAMA_MODEL=qwen2.5-3b-instruct \
      ./build/examples/17_streaming
> Tell me a short story about a curious robot.

In a world where everything hummed with technology, there was a curious robot named
Quirk. Unlike his colleagues who strictly followed their programming, Quirk loved to
explore beyond his designated tasks.

One sunny afternoon, as Quirk wandered through a botanical garden, he stumbled upon a
peculiar flower that emitted a soft, calming light. Fascinated, Quirk approached it.
The flower spoke, "Why are you so different, Quirk?"

[... full text in the run log ...]

[complete] 1294 chars
```

Tokens arrived live, not in one block at the end. Server-side timings for that request:

```
slot print_timing: prompt eval time =   134.03 ms /  29 tokens (  4.62 ms per token,  216.37 tokens per second)
slot print_timing:        eval time = 11009.63 ms / 254 tokens ( 43.52 ms per token,   22.98 tokens per second)
slot print_timing:       total time = 11143.66 ms / 283 tokens
slot print_timing:    graphs reused =      252
```

Prompt eval reads low here against the 874.8 t/s bench figure because the prompt is 29 tokens, far short of the batch size the GPU needs to fill.

## Client RSS while streaming

`VmHWM` from `/proc/<pid>/status`, polled every 50 ms for the life of the client process. `VmHWM` is the kernel's own high-water mark, so a spike between polls still shows up. This is the method the Pi 5 v0.4 run used, and the two numbers are directly comparable because of it.

Against the GPU-backed server, generating at roughly 23 tok/s:

```
run 1: exit_code=0  peak_rss_kb=5308  peak_rss_mb=5.2  wall_time_sec=12.8  [complete] 1336 chars
run 2: exit_code=0  peak_rss_kb=5144  peak_rss_mb=5.0  wall_time_sec=16.3  [complete] 1820 chars
run 3: exit_code=0  peak_rss_kb=5176  peak_rss_mb=5.1  wall_time_sec=10.8  [complete] 1153 chars
```

Against a CPU-only server (`-dev none`), generating at roughly 9 tok/s, so the same bytes arrive over three times the wall clock:

```
run 1: exit_code=0  peak_rss_kb=5116  peak_rss_mb=5.0  wall_time_sec=37.4  [complete] 1622 chars
run 2: exit_code=0  peak_rss_kb=5136  peak_rss_mb=5.0  wall_time_sec=32.1  [complete] 1448 chars
```

That server really was on the CPU:

```
slot print_timing: eval time = 36214.34 ms / 330 tokens ( 110.07 ms per token,  9.08 tokens per second)
slot print_timing: eval time = 31971.63 ms / 295 tokens ( 108.75 ms per token,  9.20 tokens per second)
```

Five runs, 5.0 to 5.2 MB, across a 2.5x spread in token rate and a 1153-to-1820-char spread in response length. Neither the backend nor the response size moves the client footprint.

### Why this is 5.1 MB when the Pi 5 is 2.0 MB

Same source at the same commit, same example, same measurement. The two candidate explanations both fail against the data above: the Pi generates at 5.5 tok/s and a slow stream here still cost 5.0 MB, and the Pi's response was 1162 chars while run 3 above was 1153 chars for 5.1 MB.

What is left is the toolchain. This board runs Ubuntu 22.04 with glibc 2.35 and g++ 11.4, the newest JetPack 6.2 offers; the Pi 5 runs Debian 13 with g++ 14.2.

```
$ ldd --version
ldd (Ubuntu GLIBC 2.35-0ubuntu3.13) 2.35

$ g++ --version
g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0
```

The agent benchmarks point the same way. The Pi 5 posts 74 ns on the static 5-deep chain against 96 ns here, and 9.4 us on the full agent stack against 10.5 us, on nominally slower silicon. Where a Jetson is being chosen for footprint or latency, a toolchain newer than the one JetPack ships is worth the trouble of installing.

An earlier pass sampled `VmRSS` every 20 ms instead and got 4.7, 5.1 and 5.1 MB over three runs. That method can only under-report, since it misses spikes between samples, and it agreed with `VmHWM` anyway. The `VmHWM` figures above are the ones quoted.

## Proof the GPU was doing the work

GPU load sampled every 100 ms through run 2, alongside the RSS sampler.

```
$ sort -n /tmp/gpuload.txt | tail -1     # max, per mille
999
$ sort -n /tmp/gpuload.txt | awk "{a[NR]=\$1} END{print a[int(NR/2)]}"   # median
999
$ grep -c . /tmp/gpuload.txt
124
```

Median 99.9% busy across the request. The GPU was saturated, not idling while the CPU worked.

## What was left on the board

`~/tiny_agent_cpp`, `~/tools` and `~/models` stay as a standing deploy for future runs, same arrangement as the Pi 5. `llama-server` was stopped and the scratch files removed.

```
$ pkill -f 'llama-serve[r]'
$ rm -f /tmp/rss-hwm.sh /tmp/rss-sample.sh /tmp/17_streaming.out /tmp/gpuload.txt \
        /tmp/ta-*.log /tmp/llama-*.log /tmp/model-dl.log /tmp/17s_stripped

$ ls /tmp | head
argus_socket
camsock
claude-1000
cuda-install.log
fastembed_cache
gdm3-config-err-CNNyX9
jiti
node-compile-cache
nvscsock
openclaw

$ ps -eo pid,comm | grep -i llama
none

$ du -sh ~/tiny_agent_cpp ~/tools ~/models
552M	/home/orin/tiny_agent_cpp
1,7G	/home/orin/tools
1,8G	/home/orin/models
```

`/tmp/cuda-install.log` is the apt transcript from the toolkit install and is owned by root, so it outlived the cleanup and will go on the next reboot. Everything else in that listing predates this run.
