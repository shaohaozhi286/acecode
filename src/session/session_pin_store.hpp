#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace acecode::session_pins {

struct PinnedSessionsState {
    std::vector<std::string> session_ids;
};

struct PinnedSessionOrderItem {
    std::string workspace_hash;
    std::string session_id;
};

struct PinnedSessionOrderState {
    std::vector<PinnedSessionOrderItem> items;
};

bool operator==(const PinnedSessionOrderItem& lhs,
                const PinnedSessionOrderItem& rhs);
bool operator!=(const PinnedSessionOrderItem& lhs,
                const PinnedSessionOrderItem& rhs);

std::vector<std::string> normalize_pinned_session_ids(
    const std::vector<std::string>& ids);
std::vector<std::string> pin_session_id(
    const std::vector<std::string>& ids,
    const std::string& session_id);
std::vector<std::string> unpin_session_id(
    const std::vector<std::string>& ids,
    const std::string& session_id);
std::vector<std::string> prune_pinned_session_ids(
    const std::vector<std::string>& ids,
    const std::vector<std::string>& available_session_ids);

PinnedSessionsState read_pinned_sessions_state(
    const std::filesystem::path& path);
bool write_pinned_sessions_state(const std::filesystem::path& path,
                                 const PinnedSessionsState& state,
                                 std::string* error = nullptr);

std::vector<PinnedSessionOrderItem> normalize_pinned_session_order_items(
    const std::vector<PinnedSessionOrderItem>& items);
std::vector<PinnedSessionOrderItem> prune_pinned_session_order_items(
    const std::vector<PinnedSessionOrderItem>& items,
    const std::vector<PinnedSessionOrderItem>& available_items);
PinnedSessionOrderState read_pinned_session_order_state(
    const std::filesystem::path& path);
bool write_pinned_session_order_state(const std::filesystem::path& path,
                                      const PinnedSessionOrderState& state,
                                      std::string* error = nullptr);

} // namespace acecode::session_pins
