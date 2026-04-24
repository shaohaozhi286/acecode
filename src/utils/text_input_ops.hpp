#pragma once

// 文本输入框的 UTF-8 光标 / 插入 / 删除纯函数。与 main.cpp 的 CatchEvent
// 分支里逐字扫描 UTF-8 continuation byte(`(c & 0xC0) == 0x80`)的行为字节级
// 等价,但抽成纯函数后可以在 acecode_testable 单测里直接覆盖,不用跑 FTXUI。
//
// 所有函数都把 `cursor` 当字节偏移处理,越界时会先 clamp 到 text.size(),
// 再执行相应操作;Backspace / Delete 在 empty buffer 或边界上是幂等 no-op,
// 不抛异常。这一组函数保持 mutex-agnostic —— 调用方负责锁的生命周期。

#include <cstddef>
#include <string>
#include <string_view>

namespace acecode {

// 在 cursor 位置插入 ch 的所有字节,并把 cursor 前进 ch.size() 个字节。
// cursor 越界时先 clamp 到 text.size()。
void insert_at_cursor(std::string& text, std::size_t& cursor, std::string_view ch);

// 删除 cursor 位置之前的一个完整 UTF-8 glyph;空串或 cursor==0 时 no-op。
void backspace_utf8(std::string& text, std::size_t& cursor);

// 删除 cursor 位置的下一个完整 UTF-8 glyph;cursor 已在末尾时 no-op。
void delete_utf8(std::string& text, std::size_t& cursor);

// 把 cursor 后退到前一个 UTF-8 glyph 的首字节;cursor==0 时 no-op。
void move_cursor_left_utf8(const std::string& text, std::size_t& cursor);

// 把 cursor 前进到下一个 UTF-8 glyph 的首字节;cursor 已在末尾时 no-op。
void move_cursor_right_utf8(const std::string& text, std::size_t& cursor);

} // namespace acecode
