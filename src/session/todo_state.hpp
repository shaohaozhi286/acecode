#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace acecode {

struct TodoItem {
    std::string id;
    std::string content;
    std::string status;
};

bool is_valid_todo_status(const std::string& status);
std::string normalize_todo_status(std::string status);

TodoItem normalize_todo_item(const nlohmann::json& item);
std::vector<TodoItem> todo_items_from_json(const nlohmann::json& raw);
nlohmann::json todo_items_to_json(const std::vector<TodoItem>& items);
nlohmann::json todo_summary_to_json(const std::vector<TodoItem>& items);
nlohmann::json todo_payload_to_json(const std::string& session_id,
                                    const std::vector<TodoItem>& items);

std::vector<TodoItem> replace_todo_items_from_json(const nlohmann::json& raw);
std::vector<TodoItem> merge_todo_items_from_json(const std::vector<TodoItem>& current,
                                                const nlohmann::json& raw);

std::string format_todo_injection(const std::vector<TodoItem>& items);

} // namespace acecode
