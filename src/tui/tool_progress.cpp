#include "tool_progress.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <chrono>
#include <string>

namespace acecode {

namespace {

// Elapsed time gate for showing the token-count segment in the waiting chip.
// Chosen to suppress flicker during very short turns (< 3s) while still
// surfacing the estimate early for anything the user actually waits on.
constexpr long SHOW_TOKENS_AFTER_MS = 3000;

std::string format_bytes(size_t n) {
    if (n < 1024) return std::to_string(n) + "B";
    if (n < 1024 * 1024) return std::to_string(n / 1024) + "KB";
    return std::to_string(n / (1024 * 1024)) + "MB";
}

long elapsed_seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
}

long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
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

    // Header line: "▌ bash: npm install…"
    std::string preview = p.command_preview;
    if (narrow && preview.size() > 20) preview = preview.substr(0, 19) + "\xE2\x80\xA6";

    Element header = hbox({
        text(" \xE2\x96\x8C ") | color(Color::GrayDark),
        text(p.tool_name + ": ") | bold | color(Color::Yellow),
        text(preview) | dim,
    });

    // No output yet → single "Running…" line
    if (p.tail_lines.empty() && p.current_partial.empty()) {
        return vbox({
            header,
            hbox({
                text("   Running\xE2\x80\xA6 ") | dim,
                text(std::to_string(secs) + "s") | dim | color(Color::CyanLight),
            }),
        });
    }

    // Tail lines (up to 5) + current partial
    Elements rows;
    rows.push_back(header);
    for (const auto& line : p.tail_lines) {
        rows.push_back(text("   " + line) | dim | color(Color::GrayLight));
    }
    if (!p.current_partial.empty()) {
        rows.push_back(text("   " + p.current_partial) | dim | color(Color::GrayLight));
    }

    // Status line
    int shown = static_cast<int>(p.tail_lines.size()) + (p.current_partial.empty() ? 0 : 1);
    int extra = std::max(0, p.total_lines - static_cast<int>(p.tail_lines.size()));
    std::string status_left;
    if (extra > 0) status_left = "+" + std::to_string(extra) + " lines";

    Elements status_row;
    if (!status_left.empty()) {
        status_row.push_back(text("   " + status_left) | dim | color(Color::GrayDark));
    } else {
        status_row.push_back(text("   ") | dim);
    }
    status_row.push_back(filler());
    status_row.push_back(text(std::to_string(secs) + "s") | dim | color(Color::CyanLight));
    if (p.total_bytes > 0) {
        status_row.push_back(text("  " + format_bytes(p.total_bytes)) | dim);
    }

    rows.push_back(hbox(std::move(status_row)));
    (void)shown;
    return vbox(std::move(rows));
}

ftxui::Element render_tool_timer_chip(const TuiState& state) {
    using namespace ftxui;
    if (!state.tool_running) return text("");
    const long secs = elapsed_seconds(state.tool_progress.start_time);
    return hbox({
        text("  \xE2\x97\x91 ") | color(Color::Yellow),
        text(state.tool_progress.tool_name) | bold | color(Color::Yellow),
        text("  " + std::to_string(secs) + "s  ") | dim | color(Color::CyanLight),
    });
}

ftxui::Element render_thinking_timer_chip(const TuiState& state) {
    using namespace ftxui;
    // Mutually exclusive with the tool timer: the tool chip wins.
    if (!state.is_waiting || state.tool_running) return text("");

    const long secs = elapsed_seconds(state.thinking_start_time);
    const long ms = elapsed_ms(state.thinking_start_time);

    // Timer segment: "  Ns  ".
    std::string timer_segment = "  " + std::to_string(secs) + "s  ";

    // Token-readout segment, only after the threshold. Prefer the
    // authoritative completion_tokens from on_usage; fall back to a
    // chars/4 estimate (tilde-prefixed). Suppress when the estimate
    // rounds to 0.
    std::string token_segment;
    if (ms >= SHOW_TOKENS_AFTER_MS) {
        if (state.last_completion_tokens_authoritative > 0) {
            token_segment = std::to_string(state.last_completion_tokens_authoritative) + " tok  ";
        } else {
            size_t est = state.streaming_output_chars / 4;
            if (est > 0) token_segment = "~" + std::to_string(est) + " tok  ";
        }
    }

    Elements parts;
    // Yellow arc glyph to mirror the tool chip's visual weight.
    parts.push_back(text("  \xE2\x97\x8B ") | color(Color::Yellow));
    // 底部 chip 强制统一显示 "Thinking"，和聊天区的轮换短语解耦——聊天动画
    // 仍然读 state.current_thinking_phrase，底部 chip 只认这个静态字面量。
    parts.push_back(text("Thinking") | bold | color(Color::Yellow));
    parts.push_back(text(timer_segment) | dim | color(Color::CyanLight));
    if (!token_segment.empty()) {
        parts.push_back(text(token_segment) | dim | color(Color::CyanLight));
    }
    return hbox(std::move(parts));
}

} // namespace acecode
