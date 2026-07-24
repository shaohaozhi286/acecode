#pragma once

#include <optional>
#include <string_view>

namespace acecode {

enum class DesktopCloseBehavior {
    Ask,
    MinimizeToTray,
    Exit,
};

inline constexpr std::string_view kDesktopCloseBehaviorAsk = "ask";
inline constexpr std::string_view kDesktopCloseBehaviorMinimizeToTray =
    "minimize_to_tray";
inline constexpr std::string_view kDesktopCloseBehaviorExit = "exit";

inline std::optional<DesktopCloseBehavior> parse_desktop_close_behavior(
    std::string_view value) {
    if (value == kDesktopCloseBehaviorAsk) {
        return DesktopCloseBehavior::Ask;
    }
    if (value == kDesktopCloseBehaviorMinimizeToTray) {
        return DesktopCloseBehavior::MinimizeToTray;
    }
    if (value == kDesktopCloseBehaviorExit) {
        return DesktopCloseBehavior::Exit;
    }
    return std::nullopt;
}

inline std::string_view desktop_close_behavior_value(
    DesktopCloseBehavior behavior) {
    switch (behavior) {
        case DesktopCloseBehavior::MinimizeToTray:
            return kDesktopCloseBehaviorMinimizeToTray;
        case DesktopCloseBehavior::Exit:
            return kDesktopCloseBehaviorExit;
        case DesktopCloseBehavior::Ask:
        default:
            return kDesktopCloseBehaviorAsk;
    }
}

inline DesktopCloseBehavior desktop_close_behavior_from_legacy(
    bool close_to_tray) {
    return close_to_tray
        ? DesktopCloseBehavior::MinimizeToTray
        : DesktopCloseBehavior::Exit;
}

inline DesktopCloseBehavior resolve_desktop_close_behavior(
    std::optional<std::string_view> configured_behavior,
    std::optional<bool> legacy_close_to_tray) {
    if (configured_behavior) {
        if (const auto parsed = parse_desktop_close_behavior(*configured_behavior)) {
            return *parsed;
        }
    }
    if (legacy_close_to_tray) {
        return desktop_close_behavior_from_legacy(*legacy_close_to_tray);
    }
    return DesktopCloseBehavior::Ask;
}

} // namespace acecode
