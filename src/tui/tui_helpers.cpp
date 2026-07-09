// TUI helper functions - implementations extracted from main.cpp anonymous namespace.
// See tui_helpers.hpp for declarations.

#include "tui/tui_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include "tui_state.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_truncation.hpp"
#include "tui/sidebar_model.hpp"
#include "tui/non_selectable.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/todo_checklist_view.hpp"
#include "tool/mcp_manager.hpp"
#include "lsp/lsp_service.hpp"

using namespace ftxui;

namespace acecode { namespace tui {

// ---- Thinking phrases ----

static const std::string EN_THINKING_PHRASES[50] = {
    "Analyzing", "Pondering", "Investigating", "Synthesizing", "Reviewing",
    "Processing", "Compiling", "Evaluating", "Formulating", "Brainstorming",
    "Searching", "Deciphering", "Gathering", "Debugging", "Inspecting",
    "Generating", "Organizing", "Mapping", "Exploring", "Tracing",
    "Validating", "Considering", "Reflecting", "Simulating", "Calculating",
    "Abstracting", "Diving", "Looking", "Troubleshooting", "Crafting",
    "Polishing", "Assembling", "Connecting", "Building", "Parsing",
    "Extracting", "Tuning", "Optimizing", "Designing", "Theorizing",
    "Hypothesizing", "Seeking", "Interpreting", "Measuring", "Weighing",
    "Reading", "Preparing", "Reasoning", "Constructing", "Finalizing"
};

static const std::string ZH_THINKING_PHRASES[50] = {
    "分析中", "思考中", "研究中", "探索中", "综合中",
    "审查中", "处理中", "编译中", "评估中", "规划中",
    "构思中", "搜索中", "解码中", "收集中", "调试中",
    "检查中", "生成中", "组织中", "映射中", "推理中",
    "验证中", "考虑中", "反思中", "模拟中", "计算中",
    "抽象中", "深挖中", "寻找中", "排查中", "打磨中",
    "完善中", "组装中", "连接中", "构建中", "解析中",
    "提取中", "微调中", "优化中", "设计中", "推论中",
    "假设中", "路线中", "解读中", "测量中", "权衡中",
    "阅读中", "准备中", "追溯中", "构造中", "总结中"
};

bool is_user_chinese(const TuiState& state) {
    if (state.conversation.empty()) return false;
    for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
        if (it->role == "user") {
            for (unsigned char c : it->content) {
                if (c >= 0xE0) return true;
            }
            return false;
        }
    }
    return false;
}

std::string get_random_thinking_phrase(bool is_zh) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 49);
    return is_zh ? ZH_THINKING_PHRASES[dis(gen)] : EN_THINKING_PHRASES[dis(gen)];
}

// ---- Text helpers ----

bool is_success_summary(const ToolSummary& s) {
    for (const auto& kv : s.metrics) {
        if (kv.first == "exit" && kv.second != "0") return false;
        if (kv.first == "aborted" && kv.second == "true") return false;
        if (kv.first == "timeout" && kv.second == "true") return false;
    }
    return true;
}

std::string renderable_tool_summary_line(const ToolSummary& s,
                                         const std::string& metric_str,
                                         int max_visual_width) {
    const std::string prefix = s.icon + " " + s.verb + " \xC2\xB7 ";
    const std::string suffix = metric_str.empty()
        ? std::string()
        : " \xC2\xB7 " + metric_str;
    return truncate_middle_segment(prefix, s.object, suffix, max_visual_width);
}

std::string collapse_sidebar_title_whitespace(std::string_view text) {
    std::string out;
    bool in_space = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            if (!out.empty() && !in_space) {
                out.push_back(' ');
            }
            in_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string first_user_message_title(const TuiState& state) {
    std::string explicit_title =
        collapse_sidebar_title_whitespace(state.current_session_title);
    if (!explicit_title.empty()) return explicit_title;
    for (const auto& msg : state.conversation) {
        if (msg.role == "user") {
            std::string title = collapse_sidebar_title_whitespace(msg.content);
            if (!title.empty()) {
                return title;
            }
        }
    }
    return std::string("New session");
}

void trim_ascii_space_suffix(std::string& text) {
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
}

std::string truncate_cells_prefix(std::string_view text, int max_cells) {
    if (max_cells <= 0) {
        return {};
    }
    std::string out;
    int used = 0;
    for (const auto& glyph : Utf8ToGlyphs(std::string(text))) {
        if (glyph.empty()) {
            continue;
        }
        const int width = std::max(0, string_width(glyph));
        if (used + width > max_cells) {
            break;
        }
        out += glyph;
        used += width;
    }
    return out;
}

std::string truncate_cells_middle_ascii(std::string_view text, int max_cells) {
    if (max_cells <= 0) {
        return {};
    }
    const std::string input(text);
    if (string_width(input) <= max_cells) {
        return input;
    }
    if (max_cells <= 3) {
        return truncate_cells_prefix(input, max_cells);
    }

    const int body_cells = max_cells - 3;
    const int head_cells = std::max(1, body_cells / 2);
    const int tail_cells = std::max(0, body_cells - head_cells);
    const auto glyphs = Utf8ToGlyphs(input);

    std::string head;
    int used_head = 0;
    for (const auto& glyph : glyphs) {
        const int width = std::max(0, string_width(glyph));
        if (used_head + width > head_cells) {
            break;
        }
        head += glyph;
        used_head += width;
    }

    std::vector<std::string> tail_glyphs;
    int used_tail = 0;
    for (std::size_t i = glyphs.size(); i > 0; --i) {
        const auto& glyph = glyphs[i - 1];
        const int width = std::max(0, string_width(glyph));
        if (used_tail + width > tail_cells) {
            break;
        }
        tail_glyphs.push_back(glyph);
        used_tail += width;
    }
    std::reverse(tail_glyphs.begin(), tail_glyphs.end());

    std::string out = head + "...";
    for (const auto& glyph : tail_glyphs) {
        out += glyph;
    }
    return out;
}

std::string format_tool_count(size_t tool_count) {
    return std::to_string(tool_count) + (tool_count == 1 ? " tool" : " tools");
}

std::string uppercase_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string repeat_utf8_glyph(const char* glyph, int count) {
    std::string out;
    if (count <= 0) {
        return out;
    }
    const std::string g(glyph);
    out.reserve(g.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out += g;
    }
    return out;
}

// ---- Sidebar rendering ----

Element sidebar_section_header(const std::string& label, int count) {
    return hbox({
        text(label) | color(theme().ui.text_muted) | dim,
        text(" " + std::to_string(count)) | color(theme().ui.text_dim) | dim,
    });
}

static std::string sidebar_change_stats_text(
    const SidebarFileChange& change) {
    std::string out;
    if (change.additions > 0) {
        out += "+" + std::to_string(change.additions);
    }
    if (change.deletions > 0) {
        if (!out.empty()) {
            out += " ";
        }
        out += "-" + std::to_string(change.deletions);
    }
    return out.empty() ? std::string("0") : out;
}

Element render_sidebar_change_row(
    const TuiState::McpSidebarServer& /*change_placeholder*/,
    int /*content_width*/) {
    // NOTE: The actual implementation uses SidebarFileChange, not McpSidebarServer.
    // This signature was declared incorrectly in the header. The real implementation
    // is called from render_regular_sidebar which uses SidebarFileChange directly.
    return emptyElement();
}

// The actual implementation used internally:
static Element render_sidebar_change_row_impl(
    const SidebarFileChange& change,
    int content_width) {
    const std::string stats_text = sidebar_change_stats_text(change);
    const int file_width =
        std::max(1, content_width - 2 - static_cast<int>(stats_text.size()) - 1);
    Elements stats_parts;
    if (change.additions > 0) {
        stats_parts.push_back(
            text("+" + std::to_string(change.additions)) |
            color(theme().semantic.success));
    }
    if (change.deletions > 0) {
        if (!stats_parts.empty()) {
            stats_parts.push_back(text(" "));
        }
        stats_parts.push_back(
            text("-" + std::to_string(change.deletions)) |
            color(theme().semantic.error));
    }
    if (stats_parts.empty()) {
        stats_parts.push_back(text("0") | color(theme().ui.text_dim) | dim);
    }

    return hbox({
        text("  ") | color(theme().ui.text_dim),
        text(truncate_cells_middle_ascii(
                 change.display_file.empty() ? change.file : change.display_file,
                 file_width)) |
            color(theme().ui.text_muted),
        filler(),
        hbox(std::move(stats_parts)),
    });
}

// ---- MCP sidebar ----

std::string mcp_state_label(McpServerState state) {
    switch (state) {
        case McpServerState::Starting:  return "starting";
        case McpServerState::Connected: return "connected";
        case McpServerState::Disabled:  return "disabled";
        case McpServerState::Failed:    return "failed";
        case McpServerState::Cancelled: return "cancelled";
        case McpServerState::TimedOut:  return "timed_out";
    }
    return "unknown";
}

Color mcp_sidebar_state_color(const std::string& state) {
    if (state == "connected") return theme().semantic.success;
    if (state == "starting") return Color::White;
    if (state == "failed" || state == "timed_out") return theme().semantic.error;
    if (state == "cancelled") return theme().semantic.warning;
    return theme().ui.text_dim;
}

bool mcp_sidebar_has_loading(
    const std::vector<TuiState::McpSidebarServer>& servers) {
    for (const auto& server : servers) {
        if (server.state == "starting") return true;
    }
    return false;
}

bool mcp_sidebar_has_loading(const TuiState& state) {
    return mcp_sidebar_has_loading(state.mcp_sidebar_servers);
}

Element render_white_shimmer_text(const std::string& label,
                                  int anim_tick,
                                  bool with_dots) {
    std::vector<std::string> glyphs = Utf8ToGlyphs(label);
    const int total = static_cast<int>(glyphs.size());
    const int wave_pos = std::max(0, anim_tick) % (total > 0 ? total + 2 : 8);

    Elements parts;
    for (int i = 0; i < total; ++i) {
        int dist = i - wave_pos;
        if (dist < 0) dist = -dist;
        Color c;
        if (dist == 0) {
            c = Color::White;
        } else if (dist == 1) {
            c = Color::GrayLight;
        } else if (dist == 2) {
            c = Color::GrayDark;
        } else {
            c = theme().ui.text_dim;
        }
        parts.push_back(text(glyphs[static_cast<std::size_t>(i)]) | color(c));
    }

    if (with_dots) {
        const int dot_count = (std::max(0, anim_tick) % 3) + 1;
        for (int i = 0; i < 3; ++i) {
            parts.push_back(
                text(".") |
                color(i < dot_count ? Color::White : theme().ui.text_dim));
        }
    }

    return hbox(std::move(parts));
}

std::vector<TuiState::McpSidebarServer>
build_mcp_sidebar_servers(const McpManager& manager) {
    auto server_infos = manager.list_servers();
    std::vector<TuiState::McpSidebarServer> out;
    out.reserve(server_infos.size());
    for (const auto& info : server_infos) {
        TuiState::McpSidebarServer server;
        server.name = info.name;
        server.state = mcp_state_label(info.state);
        server.transport = info.transport;
        server.error = info.error;
        server.tool_count = info.tool_count;
        out.push_back(std::move(server));
    }
    return out;
}

void set_mcp_sidebar_servers_locked(
    TuiState& state,
    std::vector<TuiState::McpSidebarServer> servers) {
    state.mcp_sidebar_servers = std::move(servers);
}

Element render_mcp_sidebar_section(
    const std::vector<TuiState::McpSidebarServer>& servers,
    int content_width,
    int anim_tick) {
    if (servers.empty()) {
        return emptyElement();
    }

    Elements rows;
    rows.push_back(text("MCP") | bold | color(theme().ui.text_primary));

    constexpr std::size_t kMaxServers = 8;
    std::size_t shown_servers = 0;

    for (const auto& server : servers) {
        if (shown_servers >= kMaxServers) {
            break;
        }
        ++shown_servers;

        const Color state_color = mcp_sidebar_state_color(server.state);
        const bool server_loading = server.state == "starting";
        const bool server_connected = server.state == "connected";
        const bool server_failed =
            server.state == "failed" || server.state == "timed_out";
        const std::string bullet = "\xE2\x80\xA2";

        Element status;
        if (server_loading) {
            status = render_white_shimmer_text("Loading", anim_tick);
        } else if (server_connected) {
            status = text("Connected (" + format_tool_count(server.tool_count) + ")") |
                     color(theme().ui.text_muted);
        } else if (server_failed && !server.error.empty()) {
            status = hbox({
                text(uppercase_ascii(server.transport) + " error: ") |
                    color(theme().semantic.error),
                paragraph(server.error) |
                    color(theme().ui.text_muted) | dim | flex,
            });
        } else {
            status = text(server.state) | color(state_color) | dim;
        }

        const int name_width = std::max(1, content_width / 2);
        rows.push_back(hbox({
            text("  " + bullet + " ") | color(state_color),
            text(truncate_cells_middle_ascii(server.name, name_width)) |
                bold | color(theme().ui.text_primary),
            text(" "),
            status | flex,
        }));
    }

    if (servers.size() > shown_servers) {
        rows.push_back(
            text("  +" + std::to_string(servers.size() - shown_servers) +
                 " more servers") |
            color(theme().ui.text_dim) | dim);
    }

    return vbox(std::move(rows));
}

// ---- Status bar ----

Color token_progress_color(int percent) {
    const auto& s = theme().semantic;
    if (percent <= 0) return theme().ui.text_dim;
    if (percent > 90) return s.error;
    if (percent >= 60) return s.warning;
    return s.success;
}

std::atomic<int> g_model_load_percent{-1};

Color model_load_color(int percent) {
    const auto& s = theme().semantic;
    if (percent < 0) return theme().ui.text_dim;
    if (percent > 90) return s.error;
    if (percent >= 70) return s.warning;
    return s.success;
}

Element render_model_load_chip() {
    const int percent = g_model_load_percent.load();
    if (percent < 0) return text("");
    const Color c = model_load_color(percent);
    return hbox({
        text("\xE2\x96\x81\xE2\x96\x83\xE2\x96\x85\xE2\x96\x87") | color(c),
        text(" " + std::to_string(percent) + "%  ") | color(c),
    });
}

Color status_line_color(const std::string& status_line) {
    return status_line.find("(deleted)") != std::string::npos
        ? theme().semantic.error
        : theme().ui.text_primary;
}

Element render_token_usage_chip(const TuiState& state) {
    if (state.token_status.empty()) {
        return text("");
    }

    constexpr int kBarCells = 10;
    constexpr const char* kFilled = "\xE2\x96\x88";
    constexpr const char* kEmpty = "\xE2\x96\x91";

    const int percent = std::clamp(state.token_percent, 0, 100);
    const int filled = percent <= 0 ? 0 : std::clamp((percent + 9) / 10, 1, kBarCells);
    const int empty = kBarCells - filled;
    const Color progress_color = token_progress_color(percent);

    return hbox({
        text("  " + state.token_status + " ") | dim | color(theme().ui.accent_alt),
        text("[") | dim | color(theme().ui.text_dim),
        text(repeat_utf8_glyph(kFilled, filled)) | color(progress_color),
        text(repeat_utf8_glyph(kEmpty, empty)) | dim | color(theme().ui.text_dim),
        text("] ") | dim | color(theme().ui.text_dim),
        text(std::to_string(percent) + "%  ") | dim | color(progress_color),
    });
}

Element queued_badge() {
    return text(" QUEUED ") | bold | color(theme().ui.text_primary) |
           bgcolor(theme().ui.queued_bg);
}

Element render_pending_queue_block(const TuiState& state, int available_width) {
    if (state.pending_queue.empty()) {
        return emptyElement();
    }

    constexpr std::size_t kMaxVisibleQueuedPrompts = 3;
    constexpr int kBadgeCells = 8;
    const int prompt_width = std::max(10, available_width - kBadgeCells - 5);
    const std::size_t visible =
        std::min(kMaxVisibleQueuedPrompts, state.pending_queue.size());

    Elements rows;
    const std::size_t hidden =
        state.pending_queue.size() > visible
            ? state.pending_queue.size() - visible
            : 0;
    if (hidden > 0) {
        rows.push_back(
            text("  +" + std::to_string(hidden) + " more queued") |
            color(theme().ui.text_dim) | dim);
    }

    const std::size_t start = state.pending_queue.size() - visible;
    for (std::size_t i = start; i < state.pending_queue.size(); ++i) {
        const std::string preview = collapse_sidebar_title_whitespace(
            state.pending_queue[i]);
        rows.push_back(hbox({
            text(" "),
            queued_badge(),
            text(" "),
            text(truncate_cells_middle_ascii(preview, prompt_width)) |
                color(theme().ui.text_primary),
        }));
    }

    return vbox(std::move(rows));
}

Element render_pending_attachment_block(const TuiState& state, int available_width) {
    if (state.pending_attachments.empty()) {
        return emptyElement();
    }

    Elements rows;
    const int label_width = std::max(12, available_width - 18);
    const bool attachment_focus = has_pending_attachment_focus(
        state.pending_attachment_focus,
        state.pending_attachments.size());
    for (std::size_t i = 0; i < state.pending_attachments.size(); ++i) {
        const auto& attachment = state.pending_attachments[i];
        const bool focused = attachment_focus &&
            state.pending_attachment_focus == static_cast<int>(i);
        const std::string kind = attachment.value("kind", std::string{"file"});
        const std::string name = attachment.value("name", std::string{"attachment"});
        const std::string prefix = kind == "image" ? " image " : " file ";
        Element row = hbox({
            text(focused ? ">" : " "),
            text(prefix) | bold |
                color(focused ? theme().ui.selection_fg : theme().ui.badge_fg) |
                bgcolor(focused ? theme().ui.selection_bg : theme().ui.badge_bg),
            text(" "),
            text(truncate_cells_middle_ascii(name, label_width)) |
                color(focused ? theme().ui.selection_fg : theme().ui.text_primary),
        });
        if (focused) {
            row = row | bgcolor(theme().ui.selection_bg);
        }
        rows.push_back(std::move(row));
    }
    const std::string hint = attachment_focus
        ? "  Up/Down: select  Delete/Backspace: remove  Esc/Alt+A: input"
        : "  Alt+A: select attachments";
    rows.push_back(
        text(truncate_cells_middle_ascii(hint, std::max(12, available_width - 2))) |
        dim | color(theme().ui.text_dim));
    return vbox(std::move(rows));
}

std::vector<std::string> sidebar_title_lines(const std::string& title, int max_width) {
    max_width = std::max(1, max_width);
    const auto glyphs = Utf8ToGlyphs(title);
    std::vector<std::string> lines;
    std::size_t index = 0;

    for (int line_index = 0; line_index < 2 && index < glyphs.size(); ++line_index) {
        std::string line;
        int width = 0;
        while (index < glyphs.size()) {
            const auto& glyph = glyphs[index];
            const int glyph_width = std::max(0, string_width(glyph));
            if (width > 0 && width + glyph_width > max_width) {
                break;
            }
            if (width == 0 && glyph_width > max_width) {
                line += glyph;
                ++index;
                break;
            }
            line += glyph;
            width += glyph_width;
            ++index;
        }
        trim_ascii_space_suffix(line);
        lines.push_back(std::move(line));
        while (index < glyphs.size() && glyphs[index] == " ") {
            ++index;
        }
    }

    if (lines.empty()) {
        lines.push_back("New session");
    }
    if (index < glyphs.size()) {
        if (lines.size() == 1) {
            lines.push_back("");
        }
        const int body_width = std::max(0, max_width - 3);
        lines[1] = truncate_cells_prefix(lines[1], body_width);
        trim_ascii_space_suffix(lines[1]);
        lines[1] += "...";
    }
    return lines;
}

Element render_regular_sidebar(const TuiState& state,
                               const std::string& version_str,
                               const std::string& cwd_display,
                               int sidebar_width,
                               int anim_tick) {
    const int content_width = std::max(1, sidebar_width - 2);
    Elements top_rows;
    for (const auto& line : sidebar_title_lines(first_user_message_title(state),
                                                content_width)) {
        top_rows.push_back(text(line) | bold | color(theme().ui.text_primary));
    }

    Element mcp_section = render_mcp_sidebar_section(
        state.mcp_sidebar_servers, content_width, anim_tick);
    if (!state.mcp_sidebar_servers.empty()) {
        top_rows.push_back(text(""));
        top_rows.push_back(std::move(mcp_section));
    }

    // LSP 状态节(openspec add-lsp-service):有已连接 server 才渲染。
    // connected_snapshot 只做锁 + 小拷贝,每帧调用安全(不做 which 探测)。
    if (lsp::is_initialized()) {
        const auto lsp_servers = lsp::service().connected_snapshot();
        if (!lsp_servers.empty()) {
            top_rows.push_back(text(""));
            top_rows.push_back(sidebar_section_header(
                "LSP", static_cast<int>(lsp_servers.size())));
            for (const auto& server : lsp_servers) {
                std::string row = server.server_id + " (" +
                                  std::to_string(server.open_files) + " files)";
                top_rows.push_back(
                    text("  ● " + truncate_cells_middle_ascii(
                                      row, std::max(1, content_width - 4))) |
                    color(theme().semantic.success));
            }
        }
    }

    const auto file_changes =
        collect_sidebar_file_changes(state.conversation, cwd_display);
    top_rows.push_back(text(""));
    top_rows.push_back(sidebar_section_header(
        "Files Changed", static_cast<int>(file_changes.size())));

    constexpr std::size_t kMaxSidebarFiles = 10;
    const std::size_t shown_files =
        std::min(kMaxSidebarFiles, file_changes.size());
    for (std::size_t i = 0; i < shown_files; ++i) {
        top_rows.push_back(
            render_sidebar_change_row_impl(file_changes[i], content_width));
    }
    if (file_changes.size() > shown_files) {
        top_rows.push_back(
            text("  +" + std::to_string(file_changes.size() - shown_files) +
                 " more") |
            color(theme().ui.text_dim) | dim);
    }

    Elements bottom_rows;
    if (!state.todos.empty()) {
        bottom_rows.push_back(
            render_todo_checklist_block(state.todos, content_width));
        bottom_rows.push_back(text(""));
    }
    const bool show_bash_task =
        state.tool_running && state.tool_progress.tool_name == "bash";
    if (show_bash_task) {
        bottom_rows.push_back(sidebar_section_header("Background Tasks", 1));
        std::string command = state.tool_progress.command_preview.empty()
            ? std::string("bash")
            : state.tool_progress.command_preview;
        bottom_rows.push_back(
            text("  " + truncate_cells_middle_ascii(command,
                                                    std::max(1, content_width - 2))) |
            color(theme().ui.text_muted));
        bottom_rows.push_back(text(""));
    }
    bottom_rows.push_back(paragraph(version_str) | color(theme().ui.text_muted) | dim);
    if (!state.update_notice.empty()) {
        bottom_rows.push_back(paragraph(state.update_notice) |
                              color(theme().semantic.warning));
    }
    if (!state.status_line.empty()) {
        bottom_rows.push_back(paragraph(state.status_line) |
                              color(status_line_color(state.status_line)));
    }
    if (!cwd_display.empty()) {
        bottom_rows.push_back(paragraph(cwd_display) | color(theme().ui.accent_alt) | dim);
    }

    const bool is_light = theme().name == "light";
    Element sidebar = hbox({
        text(" "),
        vbox({
            vbox(std::move(top_rows)),
            filler(),
            vbox(std::move(bottom_rows)),
        }) | flex,
        text(" "),
    }) | size(WIDTH, EQUAL, sidebar_width) |
       bgcolor(is_light ? Color::RGB(240, 240, 242) : Color::RGB(18, 18, 20));
    return non_selectable(std::move(sidebar));
}

// ---- Tool result ----

Element render_tool_result_lines_preserving_breaks(const std::string& display_content) {
    Elements lines;
    size_t pos = 0;
    while (pos <= display_content.size()) {
        const size_t nl = display_content.find('\n', pos);
        const std::string line = (nl == std::string::npos)
            ? display_content.substr(pos)
            : display_content.substr(pos, nl - pos);
        Element line_el = line.empty() ? text(" ") : paragraph(line);
        lines.push_back(line_el | color(theme().ui.text_muted) | dim);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return vbox(std::move(lines));
}

// ---- Input text wrapping ----

bool is_space_glyph(const std::string& glyph) {
    return glyph == " " || glyph == "\t";
}

bool is_narrow_glyph(const std::string& glyph) {
    return string_width(glyph) == 1;
}

bool is_opening_cjk_punctuation(const std::string& glyph) {
    static constexpr std::array<std::string_view, 8> kOpening = {
        "\xEF\xBC\x88",  // （
        "\xE3\x80\x8A",  // 《
        "\xE3\x80\x8C",  // 「
        "\xE3\x80\x90",  // 【
        "\xE2\x80\x98",  // '
        "\xE2\x80\x9C",  // "
        "\xE3\x80\x88",  // 〈
        "\xE3\x80\x8E",  // 『
    };
    for (const auto& candidate : kOpening) {
        if (glyph == candidate) {
            return true;
        }
    }
    return false;
}

bool is_closing_cjk_punctuation(const std::string& glyph) {
    static constexpr std::array<std::string_view, 15> kClosing = {
        "\xEF\xBC\x8C",  // ，
        "\xE3\x80\x82",  // 。
        "\xEF\xBC\x81",  // ！
        "\xEF\xBC\x9F",  // ？
        "\xEF\xBC\x9B",  // ；
        "\xEF\xBC\x9A",  // ：
        "\xE3\x80\x81",  // 、
        "\xEF\xBC\x89",  // ）
        "\xE3\x80\x8B",  // 》
        "\xE3\x80\x8D",  // 」
        "\xE3\x80\x91",  // 】
        "\xE2\x80\x99",  // '
        "\xE2\x80\x9D",  // "
        "\xE3\x80\x89",  // 〉
        "\xE3\x80\x8F",  // 』
    };
    for (const auto& candidate : kClosing) {
        if (glyph == candidate) {
            return true;
        }
    }
    return false;
}

void flush_ascii_run(std::string* ascii_run,
                     std::string* pending_prefix,
                     std::vector<std::string>* output) {
    if (ascii_run->empty()) {
        return;
    }

    std::string token = std::move(*ascii_run);
    ascii_run->clear();
    if (!pending_prefix->empty()) {
        token = std::move(*pending_prefix) + token;
        pending_prefix->clear();
    }
    output->push_back(std::move(token));
}

std::vector<std::string> tokenize_wrapped_input(const std::string& text) {
    std::vector<std::string> tokens;
    std::string ascii_run;
    std::string pending_prefix;

    for (const auto& glyph : Utf8ToGlyphs(text)) {
        if (glyph.empty()) {
            continue;
        }

        if (is_space_glyph(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            if (!tokens.empty()) {
                tokens.back() += " ";
            }
            continue;
        }

        if (is_opening_cjk_punctuation(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            pending_prefix += glyph;
            continue;
        }

        if (is_closing_cjk_punctuation(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            if (!tokens.empty()) {
                tokens.back() += glyph;
            } else if (!pending_prefix.empty()) {
                pending_prefix += glyph;
            } else {
                tokens.push_back(glyph);
            }
            continue;
        }

        if (is_narrow_glyph(glyph)) {
            ascii_run += glyph;
            continue;
        }

        flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
        std::string token = glyph;
        if (!pending_prefix.empty()) {
            token = std::move(pending_prefix) + token;
            pending_prefix.clear();
        }
        tokens.push_back(std::move(token));
    }

    flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
    if (!pending_prefix.empty()) {
        if (!tokens.empty()) {
            tokens.back() += pending_prefix;
        } else {
            tokens.push_back(std::move(pending_prefix));
        }
    }

    return tokens;
}

Element render_wrapped_input_text(const std::string& input_value, size_t cursor_bytes) {
    if (cursor_bytes > input_value.size()) cursor_bytes = input_value.size();

    std::string head = input_value.substr(0, cursor_bytes);
    std::string cursor_glyph;
    std::string tail;
    if (cursor_bytes < input_value.size()) {
        size_t next = cursor_bytes + 1;
        while (next < input_value.size() &&
               (static_cast<unsigned char>(input_value[next]) & 0xC0) == 0x80) {
            next++;
        }
        cursor_glyph = input_value.substr(cursor_bytes, next - cursor_bytes);
        tail = input_value.substr(next);
    }

    auto tokens_head = tokenize_wrapped_input(head);
    auto tokens_tail = tokenize_wrapped_input(tail);

    auto cursor_elem = text(cursor_glyph.empty() ? std::string(" ") : cursor_glyph)
                       | focusCursorBlock;

    if (tokens_head.empty() && tokens_tail.empty()) {
        return cursor_elem;
    }

    Elements parts;
    parts.reserve(tokens_head.size() + tokens_tail.size() + 1);

    for (size_t i = 0; i + 1 < tokens_head.size(); ++i) {
        parts.push_back(text(std::move(tokens_head[i])));
    }

    Elements compound;
    if (!tokens_head.empty()) {
        compound.push_back(text(std::move(tokens_head.back())));
    }
    compound.push_back(cursor_elem);
    size_t tail_start = 0;
    if (!tokens_tail.empty()) {
        compound.push_back(text(std::move(tokens_tail[0])));
        tail_start = 1;
    }
    parts.push_back(hbox(std::move(compound)));

    for (size_t i = tail_start; i < tokens_tail.size(); ++i) {
        parts.push_back(text(std::move(tokens_tail[i])));
    }

    static const auto config = FlexboxConfig().SetGap(0, 0);
    return flexbox(std::move(parts), config);
}

}} // namespace acecode::tui
