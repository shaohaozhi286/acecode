#pragma once

#include "tool_executor.hpp"

namespace acecode {

class MemoryRegistry;

// `memory_read` — tier-1 progressive disclosure for the user's persistent
// memory. Read-only. Args are all optional; see create_memory_read_tool for
// semantics:
//   {}             → full MEMORY.md index + list of {name,description,type}
//   {type}         → entries of that type as {name,description}
//   {name}         → single entry with full body
//   {name, type}   → same as {name} (type ignored when name given)
ToolImpl create_memory_read_tool(MemoryRegistry& registry,
                                 std::size_t max_index_bytes);

} // namespace acecode
