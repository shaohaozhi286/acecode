// acetui/src/app.cpp

#include "acetui/app.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "acetui/insert_history.hpp"
#include "acetui/terminal.hpp"
#include "acetui/viewport.hpp"

namespace acetui {

namespace {

std::string move_to(int row, int col) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

// 启动时把 cursor 撞到屏幕底,确保 viewport 区域有空间。
void bootstrap_to_bottom(int viewport_height) {
    std::string newlines;
    newlines.reserve(static_cast<size_t>(viewport_height) * 2);
    for (int i = 0; i < viewport_height; ++i) {
        newlines += "\r\n";
    }
    Terminal::write(newlines);
}

// 清掉 viewport 区域 + 屏底,cursor 留在 viewport 顶端那行。
void erase_viewport_and_below(const Viewport& vp) {
    Terminal::write(move_to(vp.top, 1));
    Terminal::write("\033[J");
    Terminal::write(move_to(vp.top, 1));
}

bool is_exit_key(const KeyEvent& k) {
    if (k.codepoint == key::kEsc) return true;
    if ((k.codepoint == U'c' || k.codepoint == U'C') &&
        has_mod(k.mods, Modifier::Ctrl)) {
        return true;
    }
    return false;
}

// viewport 上长 delta 行(viewport 自己向上"借"空间):用 insert_history_lines
// 在 viewport 上方滚动区 push delta 个空字符串行 — 该区域内容上滚 delta 行,
// 顶部 delta 行进入终端 scrollback,viewport.top 上方那 delta 行被释放交给
// widget。viewport.top -= delta,viewport.height += delta。
//
// 已经撞顶时(viewport.top - delta < 1)截断 — widget 高度被 cap 在屏内。
void grow_viewport(Viewport& vp, int delta) {
    if (delta <= 0) return;
    int actual = delta;
    if (vp.top - actual < 1) {
        actual = vp.top - 1;  // top 至少 1
    }
    if (actual <= 0) return;

    std::vector<std::string> blanks(static_cast<size_t>(actual), std::string{});
    insert_history_lines(vp, blanks);
    vp.top    -= actual;
    vp.height += actual;
}

// viewport 下缩 delta 行:把 [vp.top .. vp.top + delta - 1] 这 delta 行清掉
// (它们之前是 widget 上沿,现在还给上方 scrollback 区);viewport.top += delta,
// viewport.height -= delta。
//
// 注意:释放的行不进 scrollback(它们之前是 widget 内容,不是历史)。
void shrink_viewport(Viewport& vp, int delta) {
    if (delta <= 0) return;
    if (delta >= vp.height) {
        delta = vp.height - 1;  // 至少留 1 行避免 viewport 蜷成 0
    }
    if (delta <= 0) return;

    std::string out;
    for (int i = 0; i < delta; ++i) {
        out += move_to(vp.top + i, 1);
        out += "\033[K";
    }
    Terminal::write(out);
    vp.top    += delta;
    vp.height -= delta;
}

// 每个 redraw 之前调一次:重新询问 widget 的 desired_height,viewport
// 跟着上下伸缩。
void adjust_viewport_for_widget(Viewport& vp, Widget& widget) {
    int desired = widget.desired_height(vp.width);
    if (desired < 1) desired = 1;
    if (desired > vp.height) {
        grow_viewport(vp, desired - vp.height);
    } else if (desired < vp.height) {
        shrink_viewport(vp, vp.height - desired);
    }
}

}  // namespace

void AppContext::insert_history(const std::vector<std::string>& lines) const {
    insert_history_lines(viewport, lines);
}

void AppContext::request_exit() {
    exit_requested = true;
}

int App::run(Widget& widget,
             const std::vector<std::string>& initial_history) {
    if (!Terminal::enable_raw()) {
        std::fprintf(
            stderr,
            "acetui: 无法启用终端 raw mode + VT processing,当前终端不支持 "
            "DECSTBM/VT,无法启动。\n");
        return 1;
    }

    AppContext ctx;
    Size screen          = Terminal::size();
    const int initial_h  = widget.desired_height(screen.width);
    ctx.viewport         = Viewport::bottom(screen, initial_h);

    bootstrap_to_bottom(ctx.viewport.height);

    // 启动前 push initial history(欢迎卡片 / tip 行等)。每行作为独立
    // 历史行,走滚动区一行一行往上滚 — 跟用户后续 Enter 提交走的是同一
    // 条路径,行为一致。
    if (!initial_history.empty()) {
        insert_history_lines(ctx.viewport, initial_history);
    }

    widget.render(ctx);

    while (true) {
        auto ev_opt = Terminal::read_event();
        if (!ev_opt) {
            continue;
        }
        const Event& ev = *ev_opt;

        // Esc / Ctrl+C → 退出
        if (const auto* key = std::get_if<KeyEvent>(&ev)) {
            if (is_exit_key(*key)) {
                break;
            }
        }

        // Resize:重算 viewport,清新区,重画。
        if (const auto* rs = std::get_if<ResizeEvent>(&ev)) {
            int new_h = widget.desired_height(rs->width);
            ctx.viewport = Viewport::bottom(Size{rs->width, rs->height}, new_h);
            erase_viewport_and_below(ctx.viewport);
            widget.render(ctx);
            (void)widget.on_event(ev, ctx);
            continue;
        }

        EventResult r = widget.on_event(ev, ctx);
        if (r == EventResult::ExitRequested || ctx.exit_requested) {
            break;
        }
        if (r == EventResult::Redraw) {
            adjust_viewport_for_widget(ctx.viewport, widget);
            widget.render(ctx);
        }
    }

    erase_viewport_and_below(ctx.viewport);
    Terminal::restore();
    return 0;
}

}  // namespace acetui
