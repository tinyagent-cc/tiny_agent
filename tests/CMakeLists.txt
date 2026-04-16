find_package(libenvpp CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator REQUIRED)

# ── Unit tests (offline, no API keys needed) ─────────────────────────────────

set(UNIT_TESTS
    test_types
    test_tool
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
