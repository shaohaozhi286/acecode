#include "pinned_sessions_handler.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace acecode::web {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string pinned_order_key(const PinnedSessionOrderItem& item) {
    return item.workspace_hash + '\0' + item.session_id;
}

} // namespace

bool operator==(const PinnedSessionOrderItem& lhs,
                const PinnedSessionOrderItem& rhs) {
    return lhs.workspace_hash == rhs.workspace_hash &&
           lhs.session_id == rhs.session_id;
}

bool operator!=(const PinnedSessionOrderItem& lhs,
                const PinnedSessionOrderItem& rhs) {
    return !(lhs == rhs);
}

std::vector<std::string> normalize_pinned_session_ids(
    const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (id.empty() || seen.count(id)) continue;
        seen.insert(id);
        out.push_back(id);
    }
    return out;
}

std::vector<std::string> pin_session_id(
    const std::vector<std::string>& ids,
    const std::string& session_id) {
    if (session_id.empty()) return normalize_pinned_session_ids(ids);
    auto out = normalize_pinned_session_ids(ids);
    out.erase(std::remove(out.begin(), out.end(), session_id), out.end());
    out.insert(out.begin(), session_id);
    return out;
}

std::vector<std::string> unpin_session_id(
    const std::vector<std::string>& ids,
    const std::string& session_id) {
    auto out = normalize_pinned_session_ids(ids);
    if (session_id.empty()) return out;
    out.erase(std::remove(out.begin(), out.end(), session_id), out.end());
    return out;
}

std::vector<std::string> prune_pinned_session_ids(
    const std::vector<std::string>& ids,
    const std::vector<std::string>& available_session_ids) {
    std::unordered_set<std::string> available;
    for (const auto& id : available_session_ids) {
        if (!id.empty()) available.insert(id);
    }

    std::vector<std::string> out;
    for (const auto& id : normalize_pinned_session_ids(ids)) {
        if (available.count(id)) out.push_back(id);
    }
    return out;
}

PinnedSessionsState read_pinned_sessions_state(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};

    try {
        auto root = json::parse(in, nullptr, true, true);
        if (!root.is_object() || !root.contains("session_ids") ||
            !root["session_ids"].is_array()) {
            return {};
        }

        std::vector<std::string> ids;
        for (const auto& item : root["session_ids"]) {
            if (item.is_string()) ids.push_back(item.get<std::string>());
        }
        return {normalize_pinned_session_ids(ids)};
    } catch (...) {
        return {};
    }
}

bool write_pinned_sessions_state(const fs::path& path,
                                 const PinnedSessionsState& state,
                                 std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error) *error = ec.message();
            return false;
        }
    }

    const auto ids = normalize_pinned_session_ids(state.session_ids);
    json root;
    root["session_ids"] = ids;

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot open state file for write";
            return false;
        }
        out << root.dump(2);
        out << '\n';
        if (!out) {
            if (error) *error = "failed to write state file";
            return false;
        }
    }

    fs::remove(path, ec);
    ec.clear();
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp);
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

std::vector<PinnedSessionOrderItem> normalize_pinned_session_order_items(
    const std::vector<PinnedSessionOrderItem>& items) {
    std::vector<PinnedSessionOrderItem> out;
    std::unordered_set<std::string> seen;
    out.reserve(items.size());
    for (const auto& item : items) {
        if (item.workspace_hash.empty() || item.session_id.empty()) continue;
        const auto key = pinned_order_key(item);
        if (seen.count(key)) continue;
        seen.insert(key);
        out.push_back(item);
    }
    return out;
}

std::vector<PinnedSessionOrderItem> prune_pinned_session_order_items(
    const std::vector<PinnedSessionOrderItem>& items,
    const std::vector<PinnedSessionOrderItem>& available_items) {
    std::unordered_set<std::string> available;
    for (const auto& item : available_items) {
        if (item.workspace_hash.empty() || item.session_id.empty()) continue;
        available.insert(pinned_order_key(item));
    }

    std::vector<PinnedSessionOrderItem> out;
    for (const auto& item : normalize_pinned_session_order_items(items)) {
        if (available.count(pinned_order_key(item))) out.push_back(item);
    }
    return out;
}

PinnedSessionOrderState read_pinned_session_order_state(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};

    try {
        auto root = json::parse(in, nullptr, true, true);
        if (!root.is_object() || !root.contains("items") ||
            !root["items"].is_array()) {
            return {};
        }

        std::vector<PinnedSessionOrderItem> items;
        for (const auto& raw : root["items"]) {
            if (!raw.is_object()) continue;
            PinnedSessionOrderItem item;
            if (raw.contains("workspace_hash") && raw["workspace_hash"].is_string()) {
                item.workspace_hash = raw["workspace_hash"].get<std::string>();
            }
            if (raw.contains("session_id") && raw["session_id"].is_string()) {
                item.session_id = raw["session_id"].get<std::string>();
            }
            items.push_back(std::move(item));
        }
        return {normalize_pinned_session_order_items(items)};
    } catch (...) {
        return {};
    }
}

bool write_pinned_session_order_state(const fs::path& path,
                                      const PinnedSessionOrderState& state,
                                      std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error) *error = ec.message();
            return false;
        }
    }

    json root;
    root["items"] = json::array();
    for (const auto& item : normalize_pinned_session_order_items(state.items)) {
        root["items"].push_back(json{
            {"workspace_hash", item.workspace_hash},
            {"session_id", item.session_id},
        });
    }

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot open state file for write";
            return false;
        }
        out << root.dump(2);
        out << '\n';
        if (!out) {
            if (error) *error = "failed to write state file";
            return false;
        }
    }

    fs::remove(path, ec);
    ec.clear();
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp);
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

} // namespace acecode::web
