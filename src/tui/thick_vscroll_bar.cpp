#include "thick_vscroll_bar.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>

namespace acecode::tui {

namespace {

// 与 src/tool/tool_icons.hpp::tool_icon 一致的 ASCII 回退判定:
// ACECODE_ASCII_ICONS 设为非空、非 "0" 时,改用 ASCII 字符以兼容老终端。
bool ascii_icons_enabled() {
    const char* v = std::getenv("ACECODE_ASCII_ICONS");
    return v && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

class ThickVScrollBar : public ftxui::Node {
   public:
    ThickVScrollBar(ftxui::Element child, int width, ftxui::Box* out_track_box)
        : ftxui::Node({std::move(child)}),
          width_(width < 1 ? 1 : width),
          out_track_box_(out_track_box) {}

    void ComputeRequirement() override {
        // 等价于 NodeDecorator::ComputeRequirement:子节点先 compute,再把
        // requirement_ 复制成第一个子节点的,然后保留 width 列宽给滚动条。
        ftxui::Node::ComputeRequirement();
        if (!children_.empty()) {
            requirement_ = children_[0]->requirement();
        }
        requirement_.min_x += width_;
    }

    void SetBox(ftxui::Box box) override {
        // 等价于 NodeDecorator::SetBox + 收缩 width 列给子节点。
        ftxui::Node::SetBox(box);
        ftxui::Box child_box = box;
        child_box.x_max -= width_;
        if (!children_.empty()) {
            children_[0]->SetBox(child_box);
        }
    }

    void Render(ftxui::Screen& screen) override {
        // 先让子节点画自己的内容(此时 stencil 仍然是父级给的,我们之后用
        // 它来对齐滚动条列;Node::Render 不会修改 stencil)。
        ftxui::Node::Render(screen);

        const ftxui::Box& stencil = screen.stencil;

        // 复用 vscroll_indicator (external/ftxui/src/ftxui/dom/scroll_indicator.cpp:42-66)
        // 的 2× 子格精度算法。size_inner = 子节点完整请求高度,
        // size_outter = 当前可见高度。
        const int size_inner = box_.y_max - box_.y_min;
        const int size_outter = stencil.y_max - stencil.y_min + 1;

        // 滚动条占据的列范围:从右往左退 width_ 列。
        const int x_right = stencil.x_max;
        const int x_left = stencil.x_max - width_ + 1;

        // 永远先回填 track_box,无论可不可滚动 —— 调用方靠 Contain(x,y) 决定
        // 落点是不是滚动条,可滚性由 y_to_focus 自己处理。
        if (out_track_box_) {
            out_track_box_->x_min = x_left;
            out_track_box_->x_max = x_right;
            out_track_box_->y_min = stencil.y_min;
            out_track_box_->y_max = stencil.y_max;
        }

        // 内容能完全装下时不画拇指,留两列空白 —— 与上游 vscroll_indicator 行为一致。
        if (size_inner <= 0 || size_outter >= size_inner) {
            return;
        }

        int size = 2 * size_outter * size_outter / size_inner;
        size = std::max(size, 1);

        const int start_y =
            2 * stencil.y_min +
            2 * (stencil.y_min - box_.y_min) * size_outter / size_inner;

        const bool ascii = ascii_icons_enabled();
        // 视觉与上游 vscroll_indicator 完全对齐:thumb 整格 = ┃ (U+2503
        // BOX DRAWINGS HEAVY VERTICAL),半格(子格精度边界)= ╹ / ╻
        // (U+2579 / U+257B)。以前我们试过 width=2 + █ 实块拼出"粗条",
        // 但和外层 borderRounded 的 │ 边框视觉冲突;现在保留上游观感,
        // 只通过 out_track_box 暴露同一列的命中盒供鼠标层使用。
        const char* thumb_full = ascii ? "|" : "\xE2\x94\x83";
        const char* thumb_top_half = ascii ? "^" : "\xE2\x95\xB9";
        const char* thumb_bot_half = ascii ? "v" : "\xE2\x95\xBB";
        const char* empty_glyph = " ";

        for (int y = stencil.y_min; y <= stencil.y_max; ++y) {
            const int y_up = 2 * y + 0;
            const int y_down = 2 * y + 1;
            const bool up = (start_y <= y_up) && (y_up <= start_y + size);
            const bool down = (start_y <= y_down) && (y_down <= start_y + size);

            const char* thumb_char =
                up ? (down ? thumb_full : thumb_top_half)
                   : (down ? thumb_bot_half : empty_glyph);

            // 全部 width 列都画相同的字符:thumb 范围内是块字形,其余留空。
            // 两列同时填实块给出"粗条"的视觉重量,无须额外 rail。
            for (int x = x_left; x <= x_right; ++x) {
                auto& cell = screen.CellAt(x, y);
                cell.character = thumb_char;
            }
        }
    }

   private:
    int width_;
    ftxui::Box* out_track_box_;
};

} // namespace

ftxui::Element thick_vscroll_bar(ftxui::Element child,
                                 int width,
                                 ftxui::Box& out_track_box) {
    return std::make_shared<ThickVScrollBar>(std::move(child), width,
                                             &out_track_box);
}

} // namespace acecode::tui
