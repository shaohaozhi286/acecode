#include "diff_view.hpp"

#include "tool/diff_view_truncate.hpp"
#include "tool/word_diff.hpp"
#include "tui/diff_line_wrap.hpp"
#include "tui/theme_palette.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace acecode {

namespace {

// 从 tool_icons.hpp 学来的 ASCII fallback 判定,避免把 tool_icon() 拖进来后
// 传一个不相关的 tool_id。行为保持一致:env var 非空且非 "0" 即降级。
bool ascii_icons_mode() {
    const char* v = std::getenv("ACECODE_ASCII_ICONS");
    if (!v) return false;
    if (v[0] == '\0') return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
}

// Hunk 分隔符字符串。
std::string hunk_separator_glyph() {
    return ascii_icons_mode() ? std::string("...") : std::string("\xE2\x8B\xAF"); // "⋯"
}

// 浅色 / 深色 diff 背景。选色目标:
//   - 在黑底终端上足够有辨识度,绿/红色相清楚
//   - 浅色不至于干扰前景代码颜色,深色用来强调词级变化
// FTXUI 的 Color::RGB 在不支持真彩的终端会自动落到最接近的基色。
ftxui::Color bg_added_line()    { return tui::theme().diff.bg_added_line; }
ftxui::Color bg_added_word()    { return tui::theme().diff.bg_added_word; }
ftxui::Color bg_removed_line()  { return tui::theme().diff.bg_removed_line; }
ftxui::Color bg_removed_word()  { return tui::theme().diff.bg_removed_word; }

// 渲染一条非配对(Context / 无配对的 Added / Removed)的 diff 行。
// 超宽内容按显示列宽软换行成多条视觉行:首行带行号 gutter,续行 gutter
// 区留空,背景色带逐行铺满。FTXUI 的 text 元素不换行,不预切的话超长行
// 会在终端右缘被直接裁掉。
ftxui::Element render_plain_line(const DiffLine& line, int line_no_width,
                                 int available_content_width) {
    using namespace ftxui;
    std::string gutter = format_gutter(
        line.kind, line.old_line_no, line.new_line_no, line_no_width);

    auto wrapped = tui::wrap_diff_spans(
        {{line.text, false}}, available_content_width);

    Elements rows;
    for (size_t r = 0; r < wrapped.size(); ++r) {
        std::string content;
        if (!wrapped[r].spans.empty()) content = wrapped[r].spans.front().text;
        // FTXUI 的 bgcolor 只染 Element 覆盖范围,想要"满宽色带"必须自己
        // 把 content 补到目标宽度。
        int pad = available_content_width - wrapped[r].width;
        if (pad > 0) content.append(static_cast<size_t>(pad), ' ');

        // gutter 为纯 ASCII(行号 + 空格 + marker),续行按字节数补空格即可。
        std::string gutter_text =
            (r == 0) ? gutter : std::string(gutter.size(), ' ');
        Element gutter_el = text(gutter_text) | color(tui::theme().diff.gutter);
        Element content_el;
        switch (line.kind) {
            case DiffLineKind::Added:
                content_el = text(content) | bgcolor(bg_added_line());
                break;
            case DiffLineKind::Removed:
                content_el = text(content) | bgcolor(bg_removed_line());
                break;
            case DiffLineKind::Context:
                content_el = text(content) | color(tui::theme().diff.line_text);
                break;
        }
        rows.push_back(hbox({gutter_el, content_el}));
    }
    if (rows.size() == 1) return std::move(rows.front());
    return vbox(std::move(rows));
}

// 词级高亮路径:把一条带配对的 Added 或 Removed 行切成多段 text 并应用
// 不同深浅的 bgcolor。未变化部分用行级背景,变化部分用词级深色背景。
// `segs` 是 word_diff(removed_text, added_text) 的结果。
// 与 plain 路径一样按显示列宽软换行(旧实现把溢出段直接截断丢内容);
// 强调段跨行时深浅背景归属保持不变。
ftxui::Element render_word_diff_line(
    const DiffLine& line,
    const std::vector<WordDiffSegment>& segs,
    int line_no_width,
    int available_content_width
) {
    using namespace ftxui;
    const bool is_add = (line.kind == DiffLineKind::Added);
    ftxui::Color line_bg = is_add ? bg_added_line() : bg_removed_line();
    ftxui::Color word_bg = is_add ? bg_added_word() : bg_removed_word();

    // 过滤出属于本行的段:
    //   - Added 行:保留 Same 与 Added
    //   - Removed 行:保留 Same 与 Removed
    std::vector<tui::DiffWrapSpan> spans;
    for (const auto& s : segs) {
        bool show = false;
        bool emphasized = false;
        if (s.kind == WordDiffKind::Same) {
            show = true;
        } else if (is_add && s.kind == WordDiffKind::Added) {
            show = true;
            emphasized = true;
        } else if (!is_add && s.kind == WordDiffKind::Removed) {
            show = true;
            emphasized = true;
        }
        if (!show || s.value.empty()) continue;
        spans.push_back({s.value, emphasized});
    }

    auto wrapped = tui::wrap_diff_spans(spans, available_content_width);
    std::string gutter = format_gutter(
        line.kind, line.old_line_no, line.new_line_no, line_no_width);

    Elements rows;
    for (size_t r = 0; r < wrapped.size(); ++r) {
        Elements row_parts;
        std::string gutter_text =
            (r == 0) ? gutter : std::string(gutter.size(), ' ');
        row_parts.push_back(text(gutter_text) | color(tui::theme().diff.gutter));
        for (const auto& sp : wrapped[r].spans) {
            row_parts.push_back(
                text(sp.text) | bgcolor(sp.emphasized ? word_bg : line_bg));
        }
        // 右侧剩余空间用浅色行级背景填到行尾。
        int pad = available_content_width - wrapped[r].width;
        if (pad > 0) {
            row_parts.push_back(
                text(std::string(static_cast<size_t>(pad), ' ')) | bgcolor(line_bg));
        }
        rows.push_back(hbox(std::move(row_parts)));
    }
    if (rows.size() == 1) return std::move(rows.front());
    return vbox(std::move(rows));
}

// 渲染一个 hunk。负责 paired Removed/Added 的词级高亮;其它行走 plain 路径。
// `hidden_lines`: 该 hunk 被 truncate 丢弃的中间行数,>0 时在中间插入提示。
ftxui::Element render_hunk(const DiffHunk& hunk,
                           int available_width,
                           int hidden_lines,
                           double word_diff_threshold) {
    using namespace ftxui;
    int line_no_w = compute_line_no_width(hunk);
    // gutter 视觉宽度 = line_no_w + 1 (space) + 1 (marker)
    int gutter_w = line_no_w + 2;
    int content_w = available_width - gutter_w;
    if (content_w < 1) content_w = 1;

    Elements rows;
    const auto& lines = hunk.lines;

    // 先找出相邻的 Removed → Added 配对,用于词级 diff。
    // strategy:遍历,遇到连续的 Removed 序列紧接连续的 Added 序列时,
    // 把它们按索引一一对齐(和 claudecodehaha processAdjacentLines 一致)。
    std::vector<int> pair_target(lines.size(), -1); // i → 配对的索引(或 -1)

    size_t i = 0;
    while (i < lines.size()) {
        if (lines[i].kind == DiffLineKind::Removed) {
            size_t rm_start = i;
            while (i < lines.size() && lines[i].kind == DiffLineKind::Removed) ++i;
            size_t rm_end = i; // [rm_start, rm_end) 为 removed 段
            size_t ad_start = i;
            while (i < lines.size() && lines[i].kind == DiffLineKind::Added) ++i;
            size_t ad_end = i;
            size_t pairs = std::min(rm_end - rm_start, ad_end - ad_start);
            for (size_t k = 0; k < pairs; ++k) {
                pair_target[rm_start + k] = static_cast<int>(ad_start + k);
                pair_target[ad_start + k] = static_cast<int>(rm_start + k);
            }
        } else {
            ++i;
        }
    }

    // 输出折叠时的"中间省略行"插入点:保留前 hunk.lines / 2 前后各一半
    // 的哪个位置已由 truncate 阶段决定;这里只需要在行序列中某个中点插入
    // 一条省略提示。为简单计:如果 hidden_lines > 0,插在中点。
    int midpoint = static_cast<int>(lines.size()) / 2;

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        if (hidden_lines > 0 && static_cast<int>(idx) == midpoint) {
            std::string sep = hunk_separator_glyph();
            sep += " ";
            sep += std::to_string(hidden_lines);
            sep += " lines hidden";
            rows.push_back(hbox({
                text(std::string(static_cast<size_t>(gutter_w), ' ')),
                text(sep) | color(tui::theme().diff.gutter) | dim,
            }));
        }

        const DiffLine& ln = lines[idx];
        int partner = pair_target[idx];
        bool use_word_diff = false;
        std::vector<WordDiffSegment> segs;

        if (partner >= 0 && (ln.kind == DiffLineKind::Added || ln.kind == DiffLineKind::Removed)) {
            const DiffLine& other = lines[static_cast<size_t>(partner)];
            const std::string& removed_text =
                (ln.kind == DiffLineKind::Removed) ? ln.text : other.text;
            const std::string& added_text =
                (ln.kind == DiffLineKind::Added) ? ln.text : other.text;
            segs = word_diff(removed_text, added_text);
            double ratio = word_diff_change_ratio(segs);
            if (word_diff_below_threshold(ratio, word_diff_threshold) && !segs.empty()) {
                use_word_diff = true;
            }
        }

        if (use_word_diff) {
            rows.push_back(render_word_diff_line(ln, segs, line_no_w, content_w));
        } else {
            rows.push_back(render_plain_line(ln, line_no_w, content_w));
        }
    }

    return vbox(std::move(rows));
}

} // namespace

ftxui::Element render_diff_view(
    const std::vector<DiffHunk>& hunks,
    const DiffViewOptions& opts
) {
    using namespace ftxui;
    if (hunks.empty()) {
        return text("(no diff)") | color(tui::theme().diff.gutter) | dim;
    }

    int width = opts.width > 0 ? opts.width : 80;

    TruncatedDiff td;
    if (opts.expanded) {
        // 展开态:不截,直接全量。
        td.hunks = hunks;
        td.hidden_lines_per_hunk.assign(hunks.size(), 0);
        td.hidden_hunks = 0;
    } else {
        td = truncate_hunks_for_fold(hunks, opts.max_hunks, opts.max_lines_per_hunk);
    }

    Elements blocks;
    for (size_t i = 0; i < td.hunks.size(); ++i) {
        if (i > 0) {
            // hunk 之间的分隔线
            blocks.push_back(text(hunk_separator_glyph()) | color(tui::theme().diff.gutter) | dim);
        }
        int hidden = (i < td.hidden_lines_per_hunk.size())
                         ? td.hidden_lines_per_hunk[i]
                         : 0;
        blocks.push_back(render_hunk(td.hunks[i], width, hidden, opts.word_diff_threshold));
    }

    if (td.hidden_hunks > 0) {
        std::string more = hunk_separator_glyph();
        more += " ";
        more += std::to_string(td.hidden_hunks);
        more += " more hunks";
        blocks.push_back(text(more) | color(tui::theme().diff.gutter) | dim);
    }

    return vbox(std::move(blocks));
}

} // namespace acecode
