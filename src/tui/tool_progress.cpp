#include "tool_progress.hpp"
#include "tui/theme_palette.hpp"
#include "tui/tool_row_format.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <chrono>
#include <string>

namespace acecode {

namespace {

std::string format_bytes(size_t n) {
    if (n < 1024) return std::to_string(n) + "B";
    if (n < 1024 * 1024) return std::to_string(n / 1024) + "KB";
    return std::to_string(n / (1024 * 1024)) + "MB";
}

long elapsed_seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace

ftxui::Element render_tool_progress(const TuiState& state) {
    using namespace ftxui;
    if (!state.tool_running) return text("");

    const auto& p = state.tool_progress;
    const long secs = elapsed_seconds(p.start_time);
    const int term_cols = Terminal::Size().dimx;
    const bool narrow = term_cols > 0 && term_cols < 40;

    // Header line: "● Bash(npm install…)" —— 与 transcript 的工具行同款格式,
    // 执行中指示灯为灰(与配对前的 Pending 态一致)。
    std::string preview = p.command_preview;
    if (narrow && preview.size() > 20) preview = preview.substr(0, 19) + "\xE2\x80\xA6";

    const auto& th = tui::theme();
    Elements header_segs;
    header_segs.push_back(text(" \xE2\x97\x8F ") | color(th.ui.text_dim)); // "●"
    header_segs.push_back(text(tui::pascal_case_tool_name(p.tool_name))
        | bold | color(th.ui.accent));
    if (!preview.empty()) {
        header_segs.push_back(text("(") | color(th.ui.accent));
        header_segs.push_back(text(preview) | dim);
        header_segs.push_back(text(")") | color(th.ui.accent));
    }
    Element header = hbox(std::move(header_segs));

    // No output yet → single "Running…" line
    if (p.tail_lines.empty() && p.current_partial.empty()) {
        return vbox({
            header,
            hbox({
                text("   Running\xE2\x80\xA6 ") | dim,
                text(std::to_string(secs) + "s") | dim | color(th.ui.accent_alt),
            }),
        });
    }

    // Tail lines (up to 5) + current partial
    Elements rows;
    rows.push_back(header);
    for (const auto& line : p.tail_lines) {
        rows.push_back(text("   " + line) | dim | color(th.ui.text_muted));
    }
    if (!p.current_partial.empty()) {
        rows.push_back(text("   " + p.current_partial) | dim | color(th.ui.text_muted));
    }

    // Status line
    int shown = static_cast<int>(p.tail_lines.size()) + (p.current_partial.empty() ? 0 : 1);
    int extra = std::max(0, p.total_lines - static_cast<int>(p.tail_lines.size()));
    std::string status_left;
    if (extra > 0) status_left = "+" + std::to_string(extra) + " lines";

    Elements status_row;
    if (!status_left.empty()) {
        status_row.push_back(text("   " + status_left) | dim | color(th.ui.text_dim));
    } else {
        status_row.push_back(text("   ") | dim);
    }
    status_row.push_back(filler());
    status_row.push_back(text(std::to_string(secs) + "s") | dim | color(th.ui.accent_alt));
    if (p.total_bytes > 0) {
        status_row.push_back(text("  " + format_bytes(p.total_bytes)) | dim);
    }

    rows.push_back(hbox(std::move(status_row)));
    (void)shown;
    return vbox(std::move(rows));
}

} // namespace acecode
