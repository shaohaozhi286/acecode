#pragma once

#include "tool_executor.hpp"

namespace acecode {

class MemoryRegistry;

// `memory_write` — create / update / upsert a memory entry. Writes to
// ~/.acecode/memory/<name>.md atomically and updates MEMORY.md. NOT read-only.
// Auto-approved in permission flow because the write path is locked to the
// memory directory — see permissions.hpp.
ToolImpl create_memory_write_tool(MemoryRegistry& registry);

} // namespace acecode
