#pragma once

#include <string>
#include <vector>

namespace acecode::tui {

// diff 行内的一段文本 + 是否词级强调(深色背景)。plain 行整行一段。
struct DiffWrapSpan {
    std::string text;
    bool emphasized = false;
};

// 软换行后的一条视觉行:段序列 + 实际显示列宽(供右侧 padding 计算)。
struct DiffWrapRow {
    std::vector<DiffWrapSpan> spans;
    int width = 0;
};

// 把段序列按 `max_width` 显示列宽软换行成多条视觉行:
//   - glyph 级切分(ftxui::Utf8ToGlyphs),CJK 宽字符按实际列宽
//     (ftxui::string_width)计,绝不把多字节字符从中间切断;
//   - 行内相邻且 emphasized 相同的 glyph 合并回同一个 span;
//   - 输入为空(或全为空段)时仍返回一条空行,保证背景色带照常渲染;
//   - 单个 glyph 比 max_width 还宽时独占一行(该行 width 可能超出
//     max_width,调用方计算 padding 需对负值钳 0);
//   - `max_width < 1` 按 1 处理。
std::vector<DiffWrapRow> wrap_diff_spans(
    const std::vector<DiffWrapSpan>& spans,
    int max_width
);

} // namespace acecode::tui
