cmake_minimum_required(VERSION 3.20)
project(tiny_agent VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Dependencies ---
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)

# --- Header-only library ---
add_library(tiny_agent INTERFACE)
target_include_directories(tiny_agent INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tiny_agent INTERFACE
    nlohmann_json::nlohmann_json
    httplib::httplib
)
target_compile_features(tiny_agent INTERFACE cxx_std_20)

# --- Optional: hnswlib for HnswVectorStore ---
option(TINY_AGENT_HNSWLIB "Enable hnswlib vector store" OFF)
if(TINY_AGENT_HNSWLIB)
    find_package(hnswlib CONFIG REQUIRED)
    target_link_libraries(tiny_agent INTERFACE hnswlib::hnswlib)
    target_compile_definitions(tiny_agent INTERFACE TINY_AGENT_HAS_HNSWLIB)
endif()

# --- Subdirectories ---
option(TINY_AGENT_BUILD_EXAMPLES "Build examples" ON)
option(TINY_AGENT_BUILD_TESTS    "Build tests"    ON)

if(TINY_AGENT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(TINY_AGENT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(TINY_AGENT_BUILD_BENCH "Build benchmarks" ON)
if(TINY_AGENT_BUILD_BENCH)
    add_subdirectory(bench)
endif()
