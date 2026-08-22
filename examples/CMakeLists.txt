set(EXAMPLES
    01_basic_chat
    02_tool_calling
    03_nested_agents
    05_middleware
    06_deep_agent
    07_multimodal
    08_batch
    09_init_chat_model
    10_builtin_middleware
    11_skills
    12_mcp_http
    13_embeddings_retriever
    14_bind_and_kwargs
    15_llm_summarize
    16_deep_agent_custom
    17_streaming
    18_tracing
    19_vector_store
    20_context_management
)

# The MCP stdio transport is POSIX-only (transport.hpp guards it out on _WIN32,
# and 04_mcp_client uses mcp::connect_stdio), so build it on non-Windows only.
if(NOT WIN32)
    list(APPEND EXAMPLES 04_mcp_client)
endif()

foreach(ex ${EXAMPLES})
    add_executable(${ex} ${ex}.cpp)
    target_link_libraries(${ex} PRIVATE tiny_agent)
endforeach()

# ── Reflex example (optional rete_cpp integration) ────────────────────────
# RETE_INCLUDE_DIR is resolved in the top-level CMakeLists.txt; empty unless
# TINY_AGENT_RETE_DIR or TINY_AGENT_FETCH_RETE is set, same guard test_reflex uses.
if(RETE_INCLUDE_DIR)
    add_executable(21_reflex_agent 21_reflex_agent.cpp)
    target_include_directories(21_reflex_agent PRIVATE ${RETE_INCLUDE_DIR})
    target_link_libraries(21_reflex_agent PRIVATE tiny_agent)
endif()

add_subdirectory(middleware)
