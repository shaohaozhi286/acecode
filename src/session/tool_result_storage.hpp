#pragma once

#include "../provider/llm_provider.hpp"
#include "../tool/tool_executor.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace acecode {

inline constexpr const char* TOOL_RESULTS_SUBDIR = "tool-results";
inline constexpr const char* PERSISTED_OUTPUT_TAG = "<persisted-output>";
inline constexpr const char* PERSISTED_OUTPUT_CLOSING_TAG = "</persisted-output>";
inline constexpr std::size_t TOOL_RESULT_PREVIEW_BYTES = 2000;
inline constexpr std::size_t TOOL_RESULT_DEFAULT_MAX_BYTES = 50000;
inline constexpr std::size_t TOOL_RESULT_BASH_MAX_BYTES = 30000;
inline constexpr std::size_t TOOL_RESULTS_PER_BATCH_BUDGET_BYTES = 200000;

struct PersistedToolResult {
    std::string filepath;
    std::size_t original_size = 0;
    std::string preview;
    bool has_more = false;
};

struct ToolResultReplacementRecord {
    std::string tool_call_id;
    std::string replacement;
};

struct ToolResultReplacementState {
    std::set<std::string> seen_ids;
    std::map<std::string, std::string> replacements;
};

struct ToolResultBudgetOptions {
    std::size_t per_result_default_threshold_bytes = TOOL_RESULT_DEFAULT_MAX_BYTES;
    std::map<std::string, std::size_t> per_tool_result_threshold_bytes = {
        {"bash", TOOL_RESULT_BASH_MAX_BYTES},
    };
    std::size_t per_batch_budget_bytes = TOOL_RESULTS_PER_BATCH_BUDGET_BYTES;
    std::size_t preview_bytes = TOOL_RESULT_PREVIEW_BYTES;
};

struct ToolResultBudgetResult {
    std::vector<ToolResultReplacementRecord> newly_replaced;
    std::size_t replaced_size_bytes = 0;
};

std::string tool_results_dir_for_session(const std::string& project_dir,
                                         const std::string& session_id);

bool is_persisted_output_message(const std::string& content);
std::string persisted_output_filepath(const std::string& content);

PersistedToolResult persist_tool_result(const std::string& content,
                                        const std::string& tool_call_id,
                                        const std::string& tool_results_dir,
                                        std::size_t preview_bytes = TOOL_RESULT_PREVIEW_BYTES);

std::string build_large_tool_result_message(const PersistedToolResult& result,
                                            std::size_t preview_bytes = TOOL_RESULT_PREVIEW_BYTES);

ToolResultBudgetResult enforce_tool_result_budget(
    const std::vector<ToolCall>& tool_calls,
    std::vector<ToolResult>& results,
    const std::vector<bool>& result_ready,
    const std::string& tool_results_dir,
    ToolResultReplacementState& state,
    const ToolResultBudgetOptions& options = {});

ChatMessage encode_content_replacement_message(
    const std::vector<ToolResultReplacementRecord>& records);

std::vector<ToolResultReplacementRecord> decode_content_replacement_message(
    const ChatMessage& msg);

bool is_content_replacement_message(const ChatMessage& msg);

ToolResultReplacementState reconstruct_tool_result_replacement_state(
    const std::vector<ChatMessage>& messages);

int apply_tool_result_replacements(std::vector<ChatMessage>& messages,
                                   const ToolResultReplacementState& state);

} // namespace acecode
