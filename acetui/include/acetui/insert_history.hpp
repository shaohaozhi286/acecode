// acetui/insert_history.hpp — 把历史行从 widget viewport 上方推进 scrollback。
//
// 实现策略:DECSTBM 滚动区(`\x1b[<top>;<bottom>r`)+ `\r\n` 在滚动区
// 底端写入。滚动区限定在 viewport 上方那一片,所以滚动只动那一片;
// viewport 区域内的 widget 字符不被任何 escape 覆盖,无 erase + redraw
// 抖动。已经在 tuitest 上跑通过这条路;这里抽成可重用 API。

#pragma once

#include <string>
#include <vector>

#include "acetui/viewport.hpp"

namespace acetui {

// 把 lines 顺序写入 viewport 上方,每行一行,自然进入终端 scrollback。
//
// 副作用:cursor 最终位置在 viewport 上方紧邻一行的行首附近。caller 通常
// 在调用后需要把 cursor 重新定位到 widget 内合适位置(由 Widget::render
// 负责)。
//
// 当 viewport.top <= 1(没有上方空间)时,函数无副作用直接返回。
void insert_history_lines(const Viewport& viewport,
                          const std::vector<std::string>& lines);

}  // namespace acetui
