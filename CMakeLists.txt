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

# --- Install: headers, target, and a find_package(tiny_agent CONFIG) config ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS tiny_agent EXPORT tiny_agentTargets)
install(DIRECTORY include/tiny_agent DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/tiny_agent)

set(TINY_AGENT_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/tiny_agent)

install(EXPORT tiny_agentTargets
    FILE tiny_agentTargets.cmake
    NAMESPACE tiny_agent::
    DESTINATION ${TINY_AGENT_CMAKE_DIR}
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tiny_agentConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/tiny_agentConfig.cmake
    INSTALL_DESTINATION ${TINY_AGENT_CMAKE_DIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/tiny_agentConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/tiny_agentConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/tiny_agentConfigVersion.cmake
    DESTINATION ${TINY_AGENT_CMAKE_DIR}
)

# --- Optional: rete_cpp for reflex middleware (used by examples/ and tests/) ---
# Header-only rete_cpp is opt-in: point TINY_AGENT_RETE_DIR at a local checkout,
# or set TINY_AGENT_FETCH_RETE to pull it from GitHub. Neither set -> the reflex
# example and test_reflex are skipped and the build stays fully offline.
# Resolved here, not in examples/ or tests/, so both subdirectories see the
# same RETE_INCLUDE_DIR.
set(TINY_AGENT_RETE_DIR "" CACHE PATH "Local rete_cpp checkout (header-only)")
option(TINY_AGENT_FETCH_RETE "Fetch rete_cpp from GitHub for reflex middleware" OFF)

set(RETE_INCLUDE_DIR "")
if(TINY_AGENT_RETE_DIR)
  set(RETE_INCLUDE_DIR "${TINY_AGENT_RETE_DIR}/include")
elseif(TINY_AGENT_FETCH_RETE)
  include(FetchContent)
  FetchContent_Declare(rete_cpp
    GIT_REPOSITORY https://github.com/tinyagent-cc/rete_cpp.git
    GIT_TAG 4c2e6c3a94b6759bc710c26d8d601eecb3cec2e3)   # master, pinned 2026-08-22
  FetchContent_MakeAvailable(rete_cpp)
  set(RETE_INCLUDE_DIR "${rete_cpp_SOURCE_DIR}/include")
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
