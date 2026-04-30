#pragma once

// Core
#include "core/types.hpp"
#include "core/log.hpp"
#include "core/tool.hpp"
#include "core/parser.hpp"
#include "core/model.hpp"
#include "core/runnable.hpp"
#include "core/middleware.hpp"
#include "middleware/all.hpp"

// Orchestration
#include "agent.hpp"
#include "batch.hpp"

// Memory
#include "memory/store.hpp"
#include "memory/cache.hpp"

// MCP
#include "mcp/transport.hpp"
#include "mcp/client.hpp"
#include "mcp/http_transport.hpp"

// Skills
#include "skills/skill.hpp"
#include "skills/loader.hpp"
#include "skills/registry.hpp"

// Vector stores
#include "vectorstore/base.hpp"
#include "vectorstore/flat.hpp"

// Retriever
#include "retriever.hpp"

// Chat providers — include the ones you compile against:
// #include "providers/openai.hpp"
// #include "providers/anthropic.hpp"
// #include "providers/gemini.hpp"
// #include "providers/local.hpp"
//
// Or use the provider-agnostic factories (include all providers):
// #include "init_chat_model.hpp"
// #include "init_embeddings.hpp"
//
// Optional vector stores (require additional dependencies):
// #include "vectorstore/hnswlib.hpp"   // needs hnswlib
// #include "vectorstore/qdrant.hpp"    // needs running Qdrant server
