#include "permission_mode_handler.hpp"

namespace acecode::web {

std::optional<PermissionMode> parse_permission_mode_name(const std::string& name) {
    if (name == "default") return PermissionMode::Default;
    if (name == "accept-edits" || name == "acceptEdits") return PermissionMode::AcceptEdits;
    if (name == "yolo") return PermissionMode::Yolo;
    if (name == "plan") return PermissionMode::Plan;
    return std::nullopt;
}

nlohmann::json permission_mode_to_json(PermissionMode mode) {
    return nlohmann::json{
        {"mode", PermissionManager::mode_name(mode)},
        {"description", PermissionManager::mode_description(mode)},
    };
}

} // namespace acecode::web
