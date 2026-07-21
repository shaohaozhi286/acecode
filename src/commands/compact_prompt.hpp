#pragma once

#include <string>

namespace acecode {

// These strings intentionally match the pinned Codex checkpoint contract.
const std::string& get_compact_prompt();
const std::string& get_compact_summary_prefix();

// Prefix the model-produced suffix exactly as Codex stores it.
std::string get_compact_user_summary_message(const std::string& summary_text);

} // namespace acecode
