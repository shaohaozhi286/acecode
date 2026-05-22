#pragma once

#include <string>

namespace acecode {

// Provider registry for model profiles. Codex remains known so existing
// config.json/session metadata can be parsed, but it is disabled for
// selection/runtime until ACECode owns Codex tool execution and file writes.
bool is_known_model_provider(const std::string& provider);
bool is_runtime_model_provider_enabled(const std::string& provider);
const char* disabled_model_provider_reason(const std::string& provider);

} // namespace acecode
