#pragma once

// Pinned session persistence helpers for the Web/Desktop sidebar.
//
// The server stores a small ordered list of session ids per workspace project
// directory. Helpers are intentionally pure/simple so route handlers and unit
// tests can share ordering, de-duplication, pruning, and malformed-file
// behavior.

#include <filesystem>
#include <string>
#include <vector>

namespace acecode::web {

struct PinnedSessionsState {
    std::vector<std::string> session_ids;
};

// Remove empty ids and duplicates while preserving first occurrence order.
std::vector<std::string> normalize_pinned_session_ids(
    const std::vector<std::string>& ids);

// Newest-pinned-first: pinning an id moves it to the front.
std::vector<std::string> pin_session_id(
    const std::vector<std::string>& ids,
    const std::string& session_id);

std::vector<std::string> unpin_session_id(
    const std::vector<std::string>& ids,
    const std::string& session_id);

std::vector<std::string> prune_pinned_session_ids(
    const std::vector<std::string>& ids,
    const std::vector<std::string>& available_session_ids);

// Missing or malformed files return an empty state.
PinnedSessionsState read_pinned_sessions_state(const std::filesystem::path& path);

// Writes {"session_ids":[...]} to disk. Returns false on I/O failure.
bool write_pinned_sessions_state(const std::filesystem::path& path,
                                 const PinnedSessionsState& state,
                                 std::string* error = nullptr);

} // namespace acecode::web
