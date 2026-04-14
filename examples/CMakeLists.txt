set(EXAMPLES
    01_basic_chat
    02_tool_calling
    03_nested_agents
    04_mcp_client
    05_middleware
    06_deep_agent
    07_multimodal
    08_batch
    09_init_chat_model
    10_builtin_middleware
    11_skills
    12_mcp_http
)

foreach(ex ${EXAMPLES})
    add_executable(${ex} ${ex}.cpp)
    target_link_libraries(${ex} PRIVATE tiny_agent)
endforeach()

add_subdirectory(middleware)
