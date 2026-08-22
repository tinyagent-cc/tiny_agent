find_package(libenvpp CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)

# ── Reflex middleware (optional rete_cpp integration) ────────────────────────
# RETE_INCLUDE_DIR is resolved once in the top-level CMakeLists.txt (TINY_AGENT_RETE_DIR
# or TINY_AGENT_FETCH_RETE) so examples/ and tests/ share the same rete_cpp checkout.
# Neither set -> RETE_INCLUDE_DIR is empty, test_reflex is skipped, and the suite
# stays fully offline.

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

# test_readme_snippets compiles every code block in README.md verbatim; the
# reflex block is optional like everything else rete_cpp, so it only builds
# when RETE_INCLUDE_DIR is set (see top-level CMakeLists.txt) rather than
# making rete_cpp a hard dependency of the always-on offline suite.
if(RETE_INCLUDE_DIR)
    target_include_directories(test_readme_snippets PRIVATE ${RETE_INCLUDE_DIR})
    target_compile_definitions(test_readme_snippets PRIVATE TINY_AGENT_HAS_RETE)
endif()

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
