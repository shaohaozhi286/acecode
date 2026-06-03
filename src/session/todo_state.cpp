#include "todo_state.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace acecode {

namespace {

std::string trim_copy(std::string s) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    std::size_t start = 0;
    while (start < s.size() && is_space(static_cast<unsigned char>(s[start]))) ++start;
    std::size_t end = s.size();
    while (end > start && is_space(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string lower_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

std::string string_field(const nlohmann::json& item, const char* key) {
    if (!item.is_object() || !item.contains(key)) return {};
    const auto& v = item[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    return {};
}

std::vector<nlohmann::json> dedupe_by_id_keep_last_position(const nlohmann::json& raw) {
    std::vector<nlohmann::json> source;
    if (!raw.is_array()) return source;
    source.reserve(raw.size());
    std::map<std::string, std::size_t> last_index;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        source.push_back(raw[i]);
        std::string id = trim_copy(string_field(raw[i], "id"));
        if (id.empty()) id = "?";
        last_index[id] = i;
    }

    std::set<std::size_t> keep;
    for (const auto& kv : last_index) {
        keep.insert(kv.second);
    }

    std::vector<nlohmann::json> out;
    out.reserve(keep.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (keep.count(i)) out.push_back(std::move(source[i]));
    }
    return out;
}

} // namespace

bool is_valid_todo_status(const std::string& status) {
    return status == "pending" ||
           status == "in_progress" ||
           status == "completed" ||
           status == "cancelled";
}

std::string normalize_todo_status(std::string status) {
    status = lower_ascii(trim_copy(std::move(status)));
    return is_valid_todo_status(status) ? status : std::string{"pending"};
}

TodoItem normalize_todo_item(const nlohmann::json& item) {
    std::string id = trim_copy(string_field(item, "id"));
    if (id.empty()) id = "?";

    std::string content = trim_copy(string_field(item, "content"));
    if (content.empty()) content = "(no description)";

    return TodoItem{
        std::move(id),
        std::move(content),
        normalize_todo_status(string_field(item, "status")),
    };
}

std::vector<TodoItem> todo_items_from_json(const nlohmann::json& raw) {
    std::vector<TodoItem> out;
    if (!raw.is_array()) return out;
    out.reserve(raw.size());
    for (const auto& item : raw) {
        if (!item.is_object()) continue;
        out.push_back(normalize_todo_item(item));
    }
    return out;
}

nlohmann::json todo_items_to_json(const std::vector<TodoItem>& items) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : items) {
        arr.push_back({
            {"id", item.id},
            {"content", item.content},
            {"status", normalize_todo_status(item.status)},
        });
    }
    return arr;
}

nlohmann::json todo_summary_to_json(const std::vector<TodoItem>& items) {
    nlohmann::json summary = {
        {"total", static_cast<int>(items.size())},
        {"pending", 0},
        {"in_progress", 0},
        {"completed", 0},
        {"cancelled", 0},
    };
    for (const auto& item : items) {
        const std::string status = normalize_todo_status(item.status);
        summary[status] = summary.value(status, 0) + 1;
    }
    return summary;
}

nlohmann::json todo_payload_to_json(const std::string& session_id,
                                    const std::vector<TodoItem>& items) {
    nlohmann::json payload = {
        {"todos", todo_items_to_json(items)},
        {"summary", todo_summary_to_json(items)},
    };
    if (!session_id.empty()) payload["session_id"] = session_id;
    return payload;
}

std::vector<TodoItem> replace_todo_items_from_json(const nlohmann::json& raw) {
    std::vector<TodoItem> out;
    for (const auto& item : dedupe_by_id_keep_last_position(raw)) {
        if (!item.is_object()) continue;
        out.push_back(normalize_todo_item(item));
    }
    return out;
}

std::vector<TodoItem> merge_todo_items_from_json(const std::vector<TodoItem>& current,
                                                const nlohmann::json& raw) {
    std::vector<TodoItem> out = current;
    std::unordered_map<std::string, std::size_t> index_by_id;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].status = normalize_todo_status(out[i].status);
        index_by_id[out[i].id] = i;
    }

    for (const auto& item : dedupe_by_id_keep_last_position(raw)) {
        if (!item.is_object()) continue;
        std::string id = trim_copy(string_field(item, "id"));
        if (id.empty()) continue;

        auto it = index_by_id.find(id);
        if (it != index_by_id.end()) {
            TodoItem& existing = out[it->second];
            if (item.contains("content")) {
                std::string content = trim_copy(string_field(item, "content"));
                if (!content.empty()) existing.content = std::move(content);
            }
            if (item.contains("status")) {
                std::string status = normalize_todo_status(string_field(item, "status"));
                if (is_valid_todo_status(status)) existing.status = std::move(status);
            }
            continue;
        }

        TodoItem next = normalize_todo_item(item);
        index_by_id[next.id] = out.size();
        out.push_back(std::move(next));
    }
    return out;
}

std::string format_todo_injection(const std::vector<TodoItem>& items) {
    std::ostringstream oss;
    bool wrote_header = false;
    for (const auto& item : items) {
        const std::string status = normalize_todo_status(item.status);
        if (status != "pending" && status != "in_progress") continue;
        if (!wrote_header) {
            oss << "[Your active task list was preserved across context compression]\n";
            wrote_header = true;
        }
        std::string marker = "[ ]";
        if (status == "in_progress") marker = "[>]";
        oss << "- " << marker << " " << item.id << ". " << item.content
            << " (" << status << ")\n";
    }
    std::string out = oss.str();
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

} // namespace acecode
