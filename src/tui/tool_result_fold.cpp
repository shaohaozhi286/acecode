#include "tui/tool_result_fold.hpp"

#include <algorithm>
#include <utility>

#include <ftxui/screen/string.hpp>

namespace acecode::tui {

ToolResultPreview fold_tool_result_preview(const std::string& content,
                                           int max_visual_width,
                                           std::size_t max_visual_lines) {
    ToolResultPreview preview;
    if (max_visual_width <= 0 || max_visual_lines == 0) {
        preview.folded = !content.empty();
        return preview;
    }

    const auto glyphs = ftxui::Utf8ToGlyphs(content);
    std::string current_line;
    int current_width = 0;

    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const std::string& glyph = glyphs[i];
        if (glyph.empty() || glyph == "\r") {
            continue;
        }

        if (glyph == "\n") {
            preview.lines.push_back(std::move(current_line));
            current_line.clear();
            current_width = 0;
            if (preview.lines.size() == max_visual_lines) {
                // A trailing hard newline represents one more empty rendered
                // row even when no non-empty glyph follows it.
                preview.folded = true;
                return preview;
            }
            continue;
        }

        const int glyph_width = std::max(1, ftxui::string_width(glyph));
        if (!current_line.empty() &&
            current_width + glyph_width > max_visual_width) {
            preview.lines.push_back(std::move(current_line));
            current_line.clear();
            current_width = 0;
            if (preview.lines.size() == max_visual_lines) {
                preview.folded = true;
                return preview;
            }
        }

        current_line += glyph;
        current_width += glyph_width;
    }

    if (preview.lines.size() < max_visual_lines) {
        preview.lines.push_back(std::move(current_line));
    } else if (!current_line.empty()) {
        preview.folded = true;
    }
    return preview;
}

} // namespace acecode::tui
