add_executable(bench_agent bench_agent.cpp)
target_link_libraries(bench_agent PRIVATE tiny_agent)
target_compile_options(bench_agent PRIVATE -O3 -DNDEBUG)
