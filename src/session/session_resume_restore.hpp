#pragma once

// Shared resume restoration glue.
//
// `session_replay` handles the pure canonical-message -> TUI-row expansion.
// This helper handles the integration concerns that both CLI `--resume` and
// slash `/resume` need: rebuild the AgentLoop's canonical message history,
// preserve shell-mode `!cmd + tool_result` pairs as injected shell turns, and
// append replayed TUI rows into TuiState.

#include "../provider/llm_provider.hpp"

#include <vector>

namespace acecode {

class AgentLoop;
class ToolExecutor;
struct TuiState;

void append_resumed_session_messages(const std::vector<ChatMessage>& messages,
                                     TuiState& state,
                                     AgentLoop& agent_loop,
                                     const ToolExecutor& tools);

} // namespace acecode
