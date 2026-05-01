#pragma once

#include "permissions.hpp"
#include "tui/paste_handler.hpp"
#include "utils/drag_scroll.hpp"
#include "tool/tool_executor.hpp"
#include "tool/ask_user_question_tool.hpp"

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <atomic>
#include <thread>
#include <optional>

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
        // Runtime-only fields for summary-style tool_result rendering. Set by
        // on_tool_result for new tool executions; absent for legacy/resumed
        // messages (TUI then falls back to the 10-line fold path).
        std::optional<ToolSummary> summary;
        bool expanded = false;            // toggled by Ctrl+E on the focused row
        // Runtime-only compact preview for tool_call rows (mirrors
        // ChatMessage::display_override).
        std::string display_override;
        // Runtime-only 结构化 diff,由 file_edit/file_write 的 ToolResult 填充。
        // 非空时 TUI 走彩色 diff 视图;为空(老会话或非编辑类工具)走灰色 fold 路径。
        // 同样不写入 session JSONL。
        std::optional<std::vector<DiffHunk>> hunks;
    };

    std::vector<Message> conversation;
    std::string input_text;
    // Caret byte offset within input_text. Kept UTF-8-aligned by the event
    // handler (advance/retreat skips continuation bytes, insert/erase clamp
    // to valid glyph boundaries). Whenever input_text is replaced wholesale
    // (history navigation, slash commit, clear on submit) the cursor is
    // reset to 0 or size() accordingly.
    size_t input_cursor = 0;
    InputMode input_mode = InputMode::Normal;

    // 多行粘贴折叠（fix-multiline-paste-input change）。bracketed paste 状态机
    // 拦截 ESC[200~ … ESC[201~ 之间的所有 prompt 事件（含 Return / Tab），归一化
    // 后或 inline 插入 input_text，或折叠成 [Pasted text #N +M lines] 占位符。
    //   pasted_texts      — 占位符 id → 完整原文。submit 时 expand_placeholders
    //                       把占位符替换回原文喂给 agent；input_text 被清空 /
    //                       覆盖 / 提交时调用 prune_unreferenced 清孤儿。
    //   next_paste_id     — 单调自增计数（per-process，提交后复位回 1）。
    //   paste_accumulator — 状态机本身，in_paste() 期间所有 prompt 事件都不下
    //                       发到正常 Return / 字符 / Backspace / 方向键 handler。
    std::map<int, std::string> pasted_texts;
    int next_paste_id = 1;
    acecode::tui::PasteAccumulator paste_accumulator;

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

    // Tool confirmation state.
    //   confirm_focus —— overlay 当前焦点的选项下标:
    //     0 = "Yes"        (Allow,仅本次)
    //     1 = "Yes, allow all edits during this session (shift+tab)"
    //                        (AlwaysAllow,本 session 内同名工具自动通过)
    //     2 = "No"         (Deny,默认聚焦在最安全的拒绝行)
    //   每次 on_tool_confirm 翻起 confirm_pending 时 main.cpp 把它复位为 2,
    //   避免上一次的焦点泄漏到下一次确认。
    bool confirm_pending = false;
    std::string confirm_tool_name;
    std::string confirm_tool_args;
    PermissionResult confirm_result = PermissionResult::Deny;
    int confirm_focus = 2;
    std::condition_variable confirm_cv;

    // AskUserQuestion overlay state(add-ask-user-question-tool 能力)。
    // 与 confirm_pending 互斥:同一时间至多一个阻塞型工具在跑,因此二者
    // 不可能同时为 true。渲染层的优先级仍然写成 `ask > confirm`,作为
    // 显式护栏,键盘事件则在 confirm 分支之前先被 ask 分支拦截。
    //   ask_pending          — 工具线程翻起 true,TUI 完成回答 / Esc 后翻回 false
    //   ask_payload_json     — 原始参数 JSON,便于日志 / 调试
    //   ask_questions        — parse 后的题目列表,TUI 直接按此渲染
    //   ask_question_order   — question 文本的原始顺序,format_ask_answers 使用
    //   ask_result_answers   — question 文本 → answer 字符串(multi-select 已 join)
    //   ask_result_ok        — 提交成功时 true;Esc / shutdown 为 false
    //   ask_cv               — 工具线程 wait,事件线程 notify
    // 下面是 overlay 内部的 navigation 状态,仅在 ask_pending=true 期间有效:
    //   ask_current_question — 当前正在作答的题目下标
    //   ask_option_focus     — 当前题目的焦点选项下标([options.size()] 表示 "Other")
    //   ask_multi_selected   — 当前题目多选勾选标记(大小等于该题 options 数)
    //   ask_other_input_active — true 时输入框为 "Other" 自定义文本模式
    bool ask_pending = false;
    std::string ask_payload_json;
    std::vector<AskQuestion> ask_questions;
    std::vector<std::string> ask_question_order;
    std::map<std::string, std::string> ask_result_answers;
    bool ask_result_ok = false;
    std::condition_variable ask_cv;
    int ask_current_question = 0;
    int ask_option_focus = 0;
    std::vector<bool> ask_multi_selected;
    bool ask_other_input_active = false;

    // Resume session picker state
    struct ResumeItem {
        std::string id;
        std::string display; // formatted display line
    };
    bool resume_picker_active = false;
    std::vector<ResumeItem> resume_items;
    int resume_selected = 0; // currently highlighted index
    int resume_view_offset = 0; // top index of the visible viewport window
    std::function<void(const std::string& session_id)> resume_callback;

    // Rewind picker state. Target selection and restore-mode selection are
    // separate phases so Esc can step back from modes before cancelling.
    enum class RewindRestoreMode {
        CodeAndConversation,
        ConversationOnly,
        CodeOnly,
        NeverMind,
    };
    struct RewindItem {
        size_t message_index = 0;
        std::string message_uuid;
        std::string preview;
        bool has_stable_uuid = false;
        bool can_restore_code = false;
        int changed_files = 0;
        int insertions = 0;
        int deletions = 0;
        std::string display;
    };
    struct RewindModeItem {
        RewindRestoreMode mode = RewindRestoreMode::ConversationOnly;
        std::string label;
        std::string description;
    };
    bool rewind_picker_active = false;
    bool rewind_mode_active = false;
    std::vector<RewindItem> rewind_items;
    int rewind_selected = 0;
    int rewind_view_offset = 0; // top index of the visible viewport for the items list
    std::vector<RewindModeItem> rewind_modes;
    int rewind_mode_selected = 0;
    std::function<void(RewindItem, RewindRestoreMode)> rewind_callback;

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
    int slash_dropdown_view_offset = 0; // top index of the visible viewport
    int slash_dropdown_total_matches = 0; // full match count, equals items.size()
    bool slash_dropdown_dismissed_for_input = false;

    int chat_focus_index = -1;
    bool chat_follow_tail = true;
    bool ctrl_c_armed = false;
    std::chrono::steady_clock::time_point last_ctrl_c_time{};

    // Ctrl+C 退出确认 overlay。Ctrl+C(stdin Event::CtrlC 或 Windows
    // console_ctrl_handler 转发的 PostEvent)进入时翻起,事件 handler
    // 在最前面拦截 'y'/'Y' = 退出,其它字符键 / Esc / Enter = 取消。
    // 非字符事件(mouse / resize / custom)不拦截,否则 UI 卡住。
    bool exit_confirm_pending = false;

    // drag-autoscroll: 鼠标拖到 chat_box 顶部/底部时自动滚动并补偿 selection,
    // 让选区跟着内容走 (而不是被 FTXUI 的屏幕坐标钉死在固定位置)。
    //   drag_left_pressed       — 自己维护,因为终端在 Moved 事件中通常不带 button
    //   last_mouse_x/y          — 最近一次 motion 的屏幕坐标,anim_thread 用来分类
    //   drag_phase              — 当前阶段,由 drag_scroll::classify() 决定
    //   last_drag_scroll_at     — 时间门,控制按行滚动的节奏 (60ms/行)
    //   chat_line_offset        — 在 chat_focus_index 这条消息内的额外行偏移
    //                              用于按行滚动 (现有按消息粒度滚动不动它)
    //   pending_shift_dy        — anim_thread → 事件线程的请求,
    //                              事件线程消费时调用 screen.ShiftSelection(0, dy)
    bool drag_left_pressed = false;
    int last_mouse_x = -1;
    int last_mouse_y = -1;
    drag_scroll::Phase drag_phase = drag_scroll::Phase::Idle;
    std::chrono::steady_clock::time_point last_drag_scroll_at{};
    int chat_line_offset = 0;
    int pending_shift_dy = 0;

    // selection-anchor-compensation:每帧 Renderer 开头比较 chat_focus_index 对应
    // 消息的 box.y_min 与上一帧快照的差异,若 focus_index/line_offset 都没变(用户
    // 没主动滚动)而 y 仍然漂移,说明 layout 自身在动 —— 比如 /resume 后第一帧
    // paragraph width 测不准、第二帧才把 vbox 总高度拉到正确值导致 yframe 滚动;
    // 流式新 token 把 follow-tail 锚点上推。这种漂移期间用户拖选,FTXUI 的
    // selection_data_ 仍按屏幕物理坐标钉死,会显示成 "鼠标按下时指 A 字符,松开
    // 时框住的是 B 字符" 的错位。检测到漂移就 screen.ShiftSelection(0, dy) 把
    // selection 锚点同方向移走,与 drag-autoscroll 的补偿语义一致。
    int last_focus_index = -1;
    int last_focus_box_y = -999999;  // sentinel: 还没拍快照
    int last_chat_line_offset = 0;

    // draggable-thick-scrollbar:鼠标在加粗滚动条列上按下/拖动时进入此态,
    // 与上面的 drag_left_pressed (drag-select) 互斥 —— 一次按下要么开始
    // 选区拖拽要么开始滚动条拖拽,绝不同时。
    //   drag_scrollbar_phase    — Idle = 未在拖滚动条;Dragging = 正在拖
    //   drag_scrollbar_snapshot — 按下瞬间快照 message_line_counts,拖动期间
    //                              的 y → (focus_index, line_offset) 映射全
    //                              用这份快照,这样流式输出追加新消息时拇指
    //                              不会被指针下扯走
    enum class DragScrollbarPhase { Idle, Dragging };
    DragScrollbarPhase drag_scrollbar_phase = DragScrollbarPhase::Idle;
    std::vector<int> drag_scrollbar_snapshot;

    // Async compact state
    bool is_compacting = false;                       // protected by mu
    std::atomic<bool> compact_abort_requested{false};  // cross-thread abort signal
    std::thread compact_thread;                        // background compaction thread

    // Tool progress state (streaming-tool-progress change).
    // Lifecycle:
    //   on_tool_progress_start → tool_running=true, tool_progress populated, start_time captured
    //   on_tool_progress_update → tail_snapshot/current_partial/counters updated; PostEvent throttled
    //   on_tool_progress_end → tool_running=false, tool_progress cleared; unconditional PostEvent
    struct ToolProgress {
        std::string tool_name;
        std::string command_preview;
        std::vector<std::string> tail_lines;    // up to last 5 complete lines
        std::string current_partial;            // current line in progress (no \n yet)
        int total_lines = 0;
        size_t total_bytes = 0;
        std::chrono::steady_clock::time_point start_time;
    };
    bool tool_running = false;
    ToolProgress tool_progress;
    std::chrono::steady_clock::time_point last_tool_post_event_time{};

    // Waiting-indicator state (thinking-timer-and-tokens change). All three
    // fields are guarded by `mu`. They are only meaningful while is_waiting is
    // true and are reset when on_busy_changed(true) fires.
    //   thinking_start_time           — stamped on each busy=true transition
    //   streaming_output_chars        — UTF-8 bytes from on_delta this turn
    //   last_completion_tokens_authoritative — exact completion_tokens from
    //                                   on_usage; 0 until first usage arrives
    std::chrono::steady_clock::time_point thinking_start_time{};
    size_t streaming_output_chars = 0;
    int last_completion_tokens_authoritative = 0;

    // mouse-selection-copy: when a right-click clipboard copy fires, we stamp
    // this with "now() + 2s" and snapshot the text that was written. The
    // anim_thread polls this and restores status_line to status_line_saved
    // when the deadline passes. Guarded by `mu`. Empty deadline means no
    // pending clear.
    std::chrono::steady_clock::time_point status_line_clear_at{};
    std::string status_line_saved;

    // /title command (window-title capability). Mirror of SessionManager's
    // pending_title_ kept here so the TUI can echo the current title without
    // grabbing the session manager's lock from the render path. Kept in sync
    // by the /title command handler and the resume restore path.
    std::string current_session_title;

    std::mutex mu;
};

} // namespace acecode
