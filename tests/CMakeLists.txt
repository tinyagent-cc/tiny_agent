find_package(libenvpp CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)

# ── Reflex middleware (optional rete_cpp integration) ────────────────────────
# Header-only rete_cpp is opt-in: point TINY_AGENT_RETE_DIR at a local checkout,
# or set TINY_AGENT_FETCH_RETE to pull it from GitHub. Neither set -> test_reflex
# is skipped and the suite stays fully offline.

set(TINY_AGENT_RETE_DIR "" CACHE PATH "Local rete_cpp checkout (header-only)")
option(TINY_AGENT_FETCH_RETE "Fetch rete_cpp from GitHub for reflex tests" OFF)

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

if(RETE_INCLUDE_DIR)
  add_executable(test_reflex test_reflex.cpp)
  target_include_directories(test_reflex PRIVATE ${RETE_INCLUDE_DIR})
  target_link_libraries(test_reflex PRIVATE tiny_agent doctest::doctest)
  add_test(NAME test_reflex COMMAND test_reflex)
endif()

# ── Unit tests (offline, no API keys needed) ─────────────────────────────────

set(UNIT_TESTS
    test_types
    test_tool
    test_sse
    test_stream
    test_middleware
    test_middleware_builtins
    test_init_chat_model
    test_init_embeddings
    test_vectorstore
    test_retriever
    test_skills
    test_summarize
    test_rationalize
    test_memory
    test_hardening
    test_tracing
    test_vectorstore_remote
    test_context
    test_readme_snippets
    test_vs_weaviate
    test_vs_redis
    test_vs_milvus
)

foreach(t ${UNIT_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE tiny_agent doctest::doctest)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# ── Agent tests (real models, require API keys from .env) ────────────────────

add_executable(test_agent test_agent.cpp)
target_link_libraries(test_agent PRIVATE
    tiny_agent
    doctest::doctest
    libenvpp::libenvpp
    nlohmann_json_schema_validator::validator
)
target_compile_definitions(test_agent PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
add_test(NAME test_agent COMMAND test_agent)
