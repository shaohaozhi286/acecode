#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

struct DesktopOpenRequest {
    std::string cwd;
    std::string session_id;
};

struct DesktopOpenRequestParseResult {
    std::optional<DesktopOpenRequest> request;
    std::string error;
};

// Validate a request before it crosses a process boundary.
bool valid_desktop_open_request(const DesktopOpenRequest& request,
                                std::string* error = nullptr);

// Encode only the request arguments; callers prepend the Desktop executable.
std::vector<std::string> desktop_open_request_arguments(
    const DesktopOpenRequest& request);

// Parse a complete argv vector, including argv[0]. Unrelated Desktop flags are
// ignored so this can coexist with --webapp and platform launch arguments.
DesktopOpenRequestParseResult parse_desktop_open_request_arguments(
    const std::vector<std::string>& argv);

// Per-user one-shot handoff used when a second Desktop process finds the
// singleton already held.
std::string desktop_open_request_path();
bool publish_pending_desktop_open_request(
    const DesktopOpenRequest& request,
    std::string* error = nullptr,
    const std::string& path_override = {});
std::optional<DesktopOpenRequest> take_pending_desktop_open_request(
    std::string* error = nullptr,
    const std::string& path_override = {},
    std::int64_t minimum_created_at_unix_ms = 0);

} // namespace acecode::desktop
