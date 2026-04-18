#pragma once

#include "permissions.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <atomic>
#include <thread>

namespace acecode {

// Input mode for the prompt box. Normal is the default (text sent to the LLM
// or dispatched as a `/slash` command). Shell is entered by typing `!` on an
// empty buffer and routes the next Enter directly to BashTool without an LLM
// round-trip. See openspec/changes/add-shell-input-mode.
enum class InputMode {
    Normal,
    Shell,
};

// Add the leading mode character back when persisting an entry to input_history
// so a single history list can round-trip both modes.
inline std::string prepend_mode_prefix(const std::string& text, InputMode m) {
    if (m == InputMode::Shell) return "!" + text;
    return text;
}

// Inverse of prepend_mode_prefix: decode a history entry into (mode, text).
inline std::pair<InputMode, std::string> parse_mode_prefix(const std::string& entry) {
    if (!entry.empty() && entry[0] == '!') {
        return {InputMode::Shell, entry.substr(1)};
    }
    return {InputMode::Normal, entry};
}

struct TuiState {
    struct Message {
        std::string role;
        std::string content;
        bool is_tool = false;
    };

    std::vector<Message> conversation;
    std::string input_text;
    InputMode input_mode = InputMode::Normal;
    bool is_waiting = false;
    std::string current_thinking_phrase = "Thinking";
    std::string status_line; // for auth/provider status
    std::string token_status; // for token usage display

    // Input history for up/down navigation
    std::vector<std::string> input_history;
    int history_index = -1; // -1 = not browsing history
    std::string saved_input; // saved current input when entering history

    // Pending message queue
    std::vector<std::string> pending_queue;

    // Tool confirmation state
    bool confirm_pending = false;
    std::string confirm_tool_name;
    std::string confirm_tool_args;
    PermissionResult confirm_result = PermissionResult::Deny;
    std::condition_variable confirm_cv;

    // Resume session picker state
    struct ResumeItem {
        std::string id;
        std::string display; // formatted display line
    };
    bool resume_picker_active = false;
    std::vector<ResumeItem> resume_items;
    int resume_selected = 0; // currently highlighted index
    std::function<void(const std::string& session_id)> resume_callback;

    // Slash-command dropdown state. Set by refresh_slash_dropdown() after every
    // input_text change. active becomes true when input starts with `/`, has no
    // whitespace, no other overlay is in the way, and the dismissed flag is
    // clear. Selection index is preserved across filter updates when the same
    // command still matches. dismissed_for_input is set on Esc and cleared when
    // input leaves slash-command position (empty/no-slash/has-space).
    struct SlashDropdownItem {
        std::string name;
        std::string description;
    };
    bool slash_dropdown_active = false;
    std::vector<SlashDropdownItem> slash_dropdown_items;
    int slash_dropdown_selected = 0;
    int slash_dropdown_total_matches = 0; // full match count before truncation, for "+N more"
    bool slash_dropdown_dismissed_for_input = false;

    int chat_focus_index = -1;
    bool chat_follow_tail = true;
    bool ctrl_c_armed = false;
    std::chrono::steady_clock::time_point last_ctrl_c_time{};

    // Async compact state
    bool is_compacting = false;                       // protected by mu
    std::atomic<bool> compact_abort_requested{false};  // cross-thread abort signal
    std::thread compact_thread;                        // background compaction thread

    std::mutex mu;
};

} // namespace acecode
