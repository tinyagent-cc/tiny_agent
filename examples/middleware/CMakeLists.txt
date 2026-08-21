set(MW_EXAMPLES
    system_prompt
    pii_protection
    trim_history
    summarize
    call_limits
    context_editing
    retry_and_fallback
    custom_middleware
)

# These examples shell out through popen/pclose, which is POSIX-only.
if(NOT WIN32)
    list(APPEND MW_EXAMPLES logging rationalize full_stack)
endif()

foreach(ex ${MW_EXAMPLES})
    add_executable(mw_${ex} ${ex}.cpp)
    target_link_libraries(mw_${ex} PRIVATE tiny_agent)
endforeach()
