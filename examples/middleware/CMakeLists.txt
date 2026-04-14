set(MW_EXAMPLES
    logging
    system_prompt
    pii_protection
    trim_history
    summarize
    call_limits
    context_editing
    rationalize
    retry_and_fallback
    custom_middleware
    full_stack
)

foreach(ex ${MW_EXAMPLES})
    add_executable(mw_${ex} ${ex}.cpp)
    target_link_libraries(mw_${ex} PRIVATE tiny_agent)
endforeach()
