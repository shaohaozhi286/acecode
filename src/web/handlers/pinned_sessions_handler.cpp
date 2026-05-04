#include "pinned_sessions_handler.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace acecode::web {

namespace fs = std::filesystem;
using nlohmann::json;

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

} // namespace acecode::web
