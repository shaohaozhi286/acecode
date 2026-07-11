#include "tui/diff_line_wrap.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>

namespace acecode::tui {

std::vector<DiffWrapRow> wrap_diff_spans(
    const std::vector<DiffWrapSpan>& spans,
    int max_width
) {
    if (max_width < 1) max_width = 1;

    std::vector<DiffWrapRow> rows;
    DiffWrapRow cur;

    for (const auto& span : spans) {
        if (span.text.empty()) continue;
        const auto glyphs = ftxui::Utf8ToGlyphs(span.text);
        for (const auto& glyph : glyphs) {
            if (glyph.empty()) continue;
            // 控制字符 string_width 返回 -1,与 tool_result_fold 一致地
            // 保守按 1 列计,保证循环推进。
            const int glyph_width = std::max(1, ftxui::string_width(glyph));
            if (cur.width > 0 && cur.width + glyph_width > max_width) {
                rows.push_back(std::move(cur));
                cur = {};
            }
            if (cur.spans.empty() ||
                cur.spans.back().emphasized != span.emphasized) {
                cur.spans.push_back({std::string(), span.emphasized});
            }
            cur.spans.back().text += glyph;
            cur.width += glyph_width;
        }
    }

    // 末行(可能为空 —— 空输入也要产出一条空行让背景色带渲染)。
    rows.push_back(std::move(cur));
    return rows;
}

} // namespace acecode::tui
