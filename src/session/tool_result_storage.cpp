#include "tool_result_storage.hpp"

#include "../tool/tool_icons.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string sanitize_tool_call_id(std::string id) {
    if (id.empty()) return "tool_result";
    for (char& ch : id) {
        const bool safe =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.';
        if (!safe) ch = '_';
    }
    return id;
}

std::string tool_result_path(const std::string& tool_results_dir,
                             const std::string& tool_call_id) {
    return path_to_utf8(path_from_utf8(tool_results_dir) /
                        (sanitize_tool_call_id(tool_call_id) + ".txt"));
}

std::string generate_preview(const std::string& content,
                             std::size_t preview_bytes,
                             bool& has_more) {
    has_more = content.size() > preview_bytes;
    if (!has_more) return content;
    return truncate_utf8_prefix(content, preview_bytes, "");
}

std::size_t replacement_size_after_persist_estimate(std::size_t preview_bytes) {
    // 预览字符串还包含路径、大小说明和 XML 标签;这里保守估 1KB 头部。
    return preview_bytes + 1024;
}

struct Candidate {
    std::size_t index = 0;
    std::string tool_call_id;
    std::size_t size = 0;
};

} // namespace

std::string tool_results_dir_for_session(const std::string& project_dir,
                                         const std::string& session_id) {
    if (project_dir.empty() || session_id.empty()) return {};
    return path_to_utf8(path_from_utf8(project_dir) /
                        session_id /
                        TOOL_RESULTS_SUBDIR);
}

bool is_persisted_output_message(const std::string& content) {
    return content.rfind(PERSISTED_OUTPUT_TAG, 0) == 0;
}

PersistedToolResult persist_tool_result(const std::string& content,
                                        const std::string& tool_call_id,
                                        const std::string& tool_results_dir,
                                        std::size_t preview_bytes) {
    PersistedToolResult out;
    if (tool_results_dir.empty()) return out;

    const std::string safe_content = ensure_utf8(content);
    out.filepath = tool_result_path(tool_results_dir, tool_call_id);
    out.original_size = safe_content.size();
    out.preview = generate_preview(safe_content, preview_bytes, out.has_more);

    std::error_code ec;
    fs::create_directories(path_from_utf8(tool_results_dir), ec);
    if (ec) {
        LOG_WARN("[tool-result-storage] failed to create dir " +
                 tool_results_dir + ": " + ec.message());
        out.filepath.clear();
        return out;
    }

    const fs::path path = path_from_utf8(out.filepath);
    if (!fs::exists(path, ec)) {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) {
            LOG_WARN("[tool-result-storage] failed to open " + out.filepath);
            out.filepath.clear();
            return out;
        }
        ofs.write(safe_content.data(), static_cast<std::streamsize>(safe_content.size()));
        if (!ofs) {
            LOG_WARN("[tool-result-storage] failed to write " + out.filepath);
            out.filepath.clear();
            return out;
        }
    }

    return out;
}

std::string build_large_tool_result_message(const PersistedToolResult& result,
                                            std::size_t preview_bytes) {
    std::string message;
    message += PERSISTED_OUTPUT_TAG;
    message += "\n";
    message += "Output too large (" + format_bytes_compact(result.original_size) +
               "). Full output saved to: " + result.filepath + "\n\n";
    message += "Preview (first " + format_bytes_compact(preview_bytes) + "):\n";
    message += result.preview;
    message += result.has_more ? "\n...\n" : "\n";
    message += PERSISTED_OUTPUT_CLOSING_TAG;
    return message;
}

ToolResultBudgetResult enforce_tool_result_budget(
    const std::vector<ToolCall>& tool_calls,
    std::vector<ToolResult>& results,
    const std::vector<bool>& result_ready,
    const std::string& tool_results_dir,
    ToolResultReplacementState& state,
    const ToolResultBudgetOptions& options) {
    ToolResultBudgetResult budget;
    if (tool_results_dir.empty() || tool_calls.empty()) return budget;

    std::vector<Candidate> fresh;
    std::size_t frozen_size = 0;
    std::size_t visible_size = 0;

    const std::size_t n = std::min(tool_calls.size(), results.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= result_ready.size() || !result_ready[i]) continue;
        const std::string& id = tool_calls[i].id;
        if (id.empty()) continue;

        auto replacement_it = state.replacements.find(id);
        if (replacement_it != state.replacements.end()) {
            results[i].output = replacement_it->second;
            visible_size += replacement_it->second.size();
            continue;
        }

        if (is_persisted_output_message(results[i].output)) {
            state.seen_ids.insert(id);
            state.replacements[id] = results[i].output;
            visible_size += results[i].output.size();
            continue;
        }

        const std::size_t size = results[i].output.size();
        if (state.seen_ids.count(id)) {
            frozen_size += size;
            visible_size += size;
            continue;
        }

        fresh.push_back(Candidate{i, id, size});
        visible_size += size;
    }

    if (visible_size <= options.per_batch_budget_bytes) {
        for (const auto& candidate : fresh) {
            state.seen_ids.insert(candidate.tool_call_id);
        }
        return budget;
    }

    std::sort(fresh.begin(), fresh.end(), [](const Candidate& a, const Candidate& b) {
        return a.size > b.size;
    });

    std::set<std::string> selected_ids;
    std::size_t remaining = frozen_size;
    for (const auto& candidate : fresh) remaining += candidate.size;

    // 只替换 fresh 结果:旧结果一旦被模型见过,命运就冻结,避免 resume 或
    // 后续 turn 改变 prompt 前缀导致缓存失效。
    for (const auto& candidate : fresh) {
        if (remaining <= options.per_batch_budget_bytes) break;
        selected_ids.insert(candidate.tool_call_id);
        if (candidate.size > replacement_size_after_persist_estimate(options.preview_bytes)) {
            remaining -= candidate.size;
            remaining += replacement_size_after_persist_estimate(options.preview_bytes);
        }
    }

    for (const auto& candidate : fresh) {
        if (!selected_ids.count(candidate.tool_call_id)) {
            state.seen_ids.insert(candidate.tool_call_id);
            continue;
        }

        PersistedToolResult persisted = persist_tool_result(
            results[candidate.index].output,
            candidate.tool_call_id,
            tool_results_dir,
            options.preview_bytes);

        state.seen_ids.insert(candidate.tool_call_id);
        if (persisted.filepath.empty()) {
            continue;
        }

        const std::string replacement =
            build_large_tool_result_message(persisted, options.preview_bytes);
        results[candidate.index].output = replacement;
        state.replacements[candidate.tool_call_id] = replacement;
        budget.newly_replaced.push_back(
            ToolResultReplacementRecord{candidate.tool_call_id, replacement});
        budget.replaced_size_bytes += candidate.size;
    }

    if (!budget.newly_replaced.empty()) {
        LOG_INFO("[tool-result-storage] persisted " +
                 std::to_string(budget.newly_replaced.size()) +
                 " tool result(s), replaced " +
                 format_bytes_compact(budget.replaced_size_bytes));
    }

    return budget;
}

ChatMessage encode_content_replacement_message(
    const std::vector<ToolResultReplacementRecord>& records) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Content replacement records]";
    msg.is_meta = true;
    msg.subtype = "content_replacement";

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& record : records) {
        arr.push_back(nlohmann::json{
            {"kind", "tool-result"},
            {"tool_call_id", record.tool_call_id},
            {"replacement", record.replacement},
        });
    }
    msg.metadata = nlohmann::json{{"replacements", arr}};
    return msg;
}

bool is_content_replacement_message(const ChatMessage& msg) {
    return msg.is_meta && msg.subtype == "content_replacement";
}

std::vector<ToolResultReplacementRecord> decode_content_replacement_message(
    const ChatMessage& msg) {
    std::vector<ToolResultReplacementRecord> records;
    if (!is_content_replacement_message(msg) || !msg.metadata.is_object()) return records;
    const auto it = msg.metadata.find("replacements");
    if (it == msg.metadata.end() || !it->is_array()) return records;

    for (const auto& item : *it) {
        if (!item.is_object()) continue;
        if (item.value("kind", std::string{}) != "tool-result") continue;
        std::string id;
        if (item.contains("tool_call_id") && item["tool_call_id"].is_string()) {
            id = item["tool_call_id"].get<std::string>();
        } else if (item.contains("toolUseId") && item["toolUseId"].is_string()) {
            id = item["toolUseId"].get<std::string>();
        }
        if (id.empty()) continue;
        if (!item.contains("replacement") || !item["replacement"].is_string()) continue;
        records.push_back(ToolResultReplacementRecord{
            std::move(id),
            item["replacement"].get<std::string>(),
        });
    }

    return records;
}

ToolResultReplacementState reconstruct_tool_result_replacement_state(
    const std::vector<ChatMessage>& messages) {
    ToolResultReplacementState state;

    for (const auto& msg : messages) {
        if (msg.role == "tool" && !msg.tool_call_id.empty()) {
            state.seen_ids.insert(msg.tool_call_id);
            if (is_persisted_output_message(msg.content)) {
                state.replacements.emplace(msg.tool_call_id, msg.content);
            }
        }
    }

    for (const auto& msg : messages) {
        for (const auto& record : decode_content_replacement_message(msg)) {
            if (state.seen_ids.count(record.tool_call_id)) {
                state.replacements[record.tool_call_id] = record.replacement;
            }
        }
    }

    return state;
}

int apply_tool_result_replacements(std::vector<ChatMessage>& messages,
                                   const ToolResultReplacementState& state) {
    int replaced = 0;
    for (auto& msg : messages) {
        if (msg.role != "tool" || msg.tool_call_id.empty()) continue;
        auto it = state.replacements.find(msg.tool_call_id);
        if (it == state.replacements.end()) continue;
        if (msg.content == it->second) continue;
        msg.content = it->second;
        replaced++;
    }
    return replaced;
}

} // namespace acecode
