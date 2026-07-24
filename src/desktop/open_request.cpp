#include "open_request.hpp"

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/utf8_path.hpp"
#include "../utils/uuid.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace acecode::desktop {
namespace {

constexpr const char* kWorkspaceArgument = "--open-workspace";
constexpr const char* kSessionArgument = "--open-session";
constexpr std::size_t kMaxCwdBytes = 32768;
constexpr std::size_t kMaxSessionIdBytes = 512;

bool has_non_whitespace(const std::string& text) {
    for (unsigned char ch : text) {
        if (std::isspace(ch) == 0) return true;
    }
    return false;
}

bool valid_text_field(const std::string& value, std::size_t max_bytes) {
    return !value.empty() &&
           value.size() <= max_bytes &&
           value.find('\0') == std::string::npos &&
           has_non_whitespace(value);
}

std::string request_path(const std::string& path_override) {
    return path_override.empty() ? desktop_open_request_path() : path_override;
}

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

bool valid_desktop_open_request(const DesktopOpenRequest& request,
                                std::string* error) {
    if (!valid_text_field(request.cwd, kMaxCwdBytes)) {
        if (error) *error = "Desktop open request has an invalid workspace path";
        return false;
    }
    if (!valid_text_field(request.session_id, kMaxSessionIdBytes)) {
        if (error) *error = "Desktop open request has an invalid session id";
        return false;
    }
    if (error) error->clear();
    return true;
}

std::vector<std::string> desktop_open_request_arguments(
    const DesktopOpenRequest& request) {
    if (!valid_desktop_open_request(request)) return {};
    return {
        kWorkspaceArgument,
        request.cwd,
        kSessionArgument,
        request.session_id,
    };
}

DesktopOpenRequestParseResult parse_desktop_open_request_arguments(
    const std::vector<std::string>& argv) {
    DesktopOpenRequestParseResult result;
    DesktopOpenRequest request;
    bool saw_workspace = false;
    bool saw_session = false;

    for (std::size_t index = argv.empty() ? 0 : 1; index < argv.size(); ++index) {
        const std::string& argument = argv[index];
        const bool is_workspace = argument == kWorkspaceArgument;
        const bool is_session = argument == kSessionArgument;
        if (!is_workspace && !is_session) continue;
        if (index + 1 >= argv.size()) {
            result.error = argument + " requires a value";
            return result;
        }

        const std::string& value = argv[++index];
        if (value == kWorkspaceArgument || value == kSessionArgument) {
            result.error = argument + " requires a value";
            return result;
        }
        if (is_workspace) {
            if (saw_workspace) {
                result.error = "duplicate " + std::string(kWorkspaceArgument);
                return result;
            }
            saw_workspace = true;
            request.cwd = value;
        } else {
            if (saw_session) {
                result.error = "duplicate " + std::string(kSessionArgument);
                return result;
            }
            saw_session = true;
            request.session_id = value;
        }
    }

    if (!saw_workspace && !saw_session) return result;
    if (!saw_workspace || !saw_session) {
        result.error =
            "Desktop open request requires both --open-workspace and --open-session";
        return result;
    }
    if (!valid_desktop_open_request(request, &result.error)) return result;
    result.request = std::move(request);
    return result;
}

std::string desktop_open_request_path() {
    return path_to_utf8(
        path_from_utf8(get_run_dir()) / "desktop-open-request.json");
}

bool publish_pending_desktop_open_request(
    const DesktopOpenRequest& request,
    std::string* error,
    const std::string& path_override) {
    std::string validation_error;
    if (!valid_desktop_open_request(request, &validation_error)) {
        if (error) *error = std::move(validation_error);
        return false;
    }

    const nlohmann::json payload{
        {"version", 2},
        {"created_at_unix_ms", now_unix_ms()},
        {"cwd", request.cwd},
        {"session_id", request.session_id},
    };
    const std::string path = request_path(path_override);
    if (!atomic_write_file(path, payload.dump(), true)) {
        if (error) *error = "failed to write pending Desktop open request";
        return false;
    }
    if (error) error->clear();
    return true;
}

std::optional<DesktopOpenRequest> take_pending_desktop_open_request(
    std::string* error,
    const std::string& path_override,
    std::int64_t minimum_created_at_unix_ms) {
    const fs::path source = path_from_utf8(request_path(path_override));
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        if (ec && error) {
            *error = "failed to inspect pending Desktop open request: " +
                     ec.message();
        } else if (error) {
            error->clear();
        }
        return std::nullopt;
    }

    fs::path claimed = source;
    claimed += ".processing-" + generate_uuid();
    fs::rename(source, claimed, ec);
    if (ec) {
        if (error) {
            *error = "failed to claim pending Desktop open request: " +
                     ec.message();
        }
        return std::nullopt;
    }

    std::optional<DesktopOpenRequest> result;
    std::string local_error;
    try {
        std::ifstream input(claimed, std::ios::binary);
        if (!input) {
            local_error = "failed to read pending Desktop open request";
        } else {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            const auto payload = nlohmann::json::parse(buffer.str());
            if (!payload.is_object() ||
                payload.value("version", 0) != 2 ||
                !payload.contains("created_at_unix_ms") ||
                !payload["created_at_unix_ms"].is_number_integer() ||
                !payload.contains("cwd") ||
                !payload["cwd"].is_string() ||
                !payload.contains("session_id") ||
                !payload["session_id"].is_string()) {
                local_error = "pending Desktop open request has an invalid schema";
            } else if (
                payload["created_at_unix_ms"].get<std::int64_t>() <
                minimum_created_at_unix_ms) {
                local_error =
                    "ignored stale pending Desktop open request";
            } else {
                DesktopOpenRequest request{
                    payload["cwd"].get<std::string>(),
                    payload["session_id"].get<std::string>(),
                };
                if (valid_desktop_open_request(request, &local_error)) {
                    result = std::move(request);
                }
            }
        }
    } catch (const std::exception& exception) {
        local_error =
            "failed to parse pending Desktop open request: " +
            std::string(exception.what());
    }

    std::error_code remove_error;
    fs::remove(claimed, remove_error);
    if (local_error.empty() && remove_error) {
        local_error =
            "failed to remove consumed Desktop open request: " +
            remove_error.message();
    }
    if (error) *error = std::move(local_error);
    return result;
}

} // namespace acecode::desktop
