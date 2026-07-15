#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace acecode::headless {

// Result of resolving caller-supplied exact names against a runtime capability
// catalog. selected/unknown preserve request order; available is sorted and
// deduplicated for deterministic diagnostics.
struct ExactNameSelection {
    std::vector<std::string> selected;
    std::vector<std::string> unknown;
    std::vector<std::string> available;

    bool valid() const { return unknown.empty(); }
};

inline ExactNameSelection select_exact_names(
    const std::vector<std::string>& requested,
    const std::vector<std::string>& available) {
    ExactNameSelection result;
    result.available = available;
    std::sort(result.available.begin(), result.available.end());
    result.available.erase(
        std::unique(result.available.begin(), result.available.end()),
        result.available.end());

    const std::unordered_set<std::string> available_set(
        result.available.begin(), result.available.end());
    std::unordered_set<std::string> selected_seen;
    std::unordered_set<std::string> unknown_seen;
    for (const auto& name : requested) {
        if (available_set.count(name) != 0) {
            if (selected_seen.insert(name).second) {
                result.selected.push_back(name);
            }
        } else if (unknown_seen.insert(name).second) {
            result.unknown.push_back(name);
        }
    }
    return result;
}

// Stable comma-separated diagnostics. Empty input is rendered as "(none)".
inline std::string format_name_list(const std::vector<std::string>& names) {
    if (names.empty()) return "(none)";
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out << ", ";
        out << names[i];
    }
    return out.str();
}

} // namespace acecode::headless
