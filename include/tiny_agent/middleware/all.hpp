#pragma once

// ── Built-in middleware — include all ────────────────────────────────────────
//
// Original four (moved from core/middleware.hpp):
#include "logging.hpp"
#include "retry.hpp"
#include "system_prompt.hpp"
#include "trim_history.hpp"

// New middleware inspired by LangChain's prebuilt middleware:
#include "model_call_limit.hpp"
#include "tool_call_limit.hpp"
#include "model_retry.hpp"
#include "model_fallback.hpp"
#include "pii.hpp"
#include "context_editing.hpp"
#include "summarize.hpp"
#include "rationalize.hpp"
