#pragma once

// Pure helpers for /api/sessions/:id/permissions. The endpoint controls the
// active session's PermissionManager mode so the Web UI permission selector is
// backed by daemon state instead of local-only UI state.

#include "../../permissions.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode::web {

// Accept the daemon's canonical names plus the legacy Web UI camelCase value.
std::optional<PermissionMode> parse_permission_mode_name(const std::string& name);

// Serialize a permission mode to the stable Web API shape:
// {"mode":"default|accept-edits|plan|yolo","description":"..."}
nlohmann::json permission_mode_to_json(PermissionMode mode);

} // namespace acecode::web
