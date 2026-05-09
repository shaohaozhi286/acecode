// acetui/src/insert_history.cpp
//
// DECSTBM scroll-region 模式实现。同样的招式我们已经在 tuitest/main.cpp
// 跑通过(Win11 conhost / Windows Terminal 验证),这里抽成可重用 API。

#include "acetui/insert_history.hpp"

#include <string>

#include "acetui/terminal.hpp"

namespace acetui {

namespace {

std::string move_to(int row, int col) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}
std::string set_scroll_region(int top, int bottom) {
    return "\033[" + std::to_string(top) + ";" + std::to_string(bottom) + "r";
}
std::string reset_scroll_region() {
    return "\033[r";
}

}  // namespace

void insert_history_lines(const Viewport& viewport,
                          const std::vector<std::string>& lines) {
    if (viewport.top <= 1 || lines.empty()) {
        // 无上方空间或无要写内容,直接返回。
        return;
    }

    const int region_bottom = viewport.top - 1;

    std::string out;
    // 1) 把 viewport 上方的整片设为滚动区。
    out += set_scroll_region(1, region_bottom);
    // 2) cursor 落到滚动区底端那行。
    out += move_to(region_bottom, 1);

    // 3) 顺序写入每一行。每行前发 \r\n,触发滚动区上滚 1 行,在底端
    //    腾出空间写新内容。第一行也要先 \r\n,因为 cursor 当前所在行
    //    可能本来就有内容,直接覆盖会破坏 scrollback。
    for (const auto& line : lines) {
        out += "\r\n";
        out += line;
    }

    // 4) 重置滚动区为全屏。
    out += reset_scroll_region();

    Terminal::write(out);
}

}  // namespace acetui
