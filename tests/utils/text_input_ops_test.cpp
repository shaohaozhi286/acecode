// 覆盖 src/utils/text_input_ops.cpp 的五个纯函数:
//   1. insert_at_cursor:空串插入、中间插入多字节 UTF-8、cursor 越界先 clamp
//   2. backspace_utf8:单字节 / 多字节 glyph 回退、空串 no-op、cursor=0 no-op
//   3. delete_utf8:单字节 / 多字节 / cursor 已在末尾的对称行为
//   4. move_cursor_left_utf8 / move_cursor_right_utf8:UTF-8 glyph 边界移动
//   5. 光标越界时的 clamp 保护
// 这些函数是 AskUserQuestion Other 输入态分发的下层字节级操作,设计目标是
// 与 main.cpp CatchEvent lambda 里的 UTF-8 continuation-byte 扫描行为字节级等价。

#include <gtest/gtest.h>

#include "utils/text_input_ops.hpp"

#include <cstddef>
#include <string>

using acecode::backspace_utf8;
using acecode::delete_utf8;
using acecode::insert_at_cursor;
using acecode::move_cursor_left_utf8;
using acecode::move_cursor_right_utf8;

// 场景:空串从 cursor=0 位置插入单字节字符 'a',结果 "a" / cursor=1。
TEST(TextInputOpsInsertTest, InsertSingleCharIntoEmptyBuffer) {
    std::string text;
    std::size_t cursor = 0;
    insert_at_cursor(text, cursor, "a");
    EXPECT_EQ(text, "a");
    EXPECT_EQ(cursor, 1u);
}

// 场景:在 "ab" 的 cursor=1 位置插入多字节 UTF-8 汉字 "中"(3 字节),
// 结果应为 "a中b",cursor 前进 3 字节到位置 4。
TEST(TextInputOpsInsertTest, InsertMultiByteUtf8InMiddle) {
    std::string text = "ab";
    std::size_t cursor = 1;
    insert_at_cursor(text, cursor, "\xE4\xB8\xAD"); // "中"
    EXPECT_EQ(text, "a\xE4\xB8\xAD" "b");
    EXPECT_EQ(cursor, 4u);
}

// 场景:cursor 越界(> text.size())时先 clamp 再插入,不越写内存。
TEST(TextInputOpsInsertTest, InsertClampsCursorWhenOutOfRange) {
    std::string text = "hi";
    std::size_t cursor = 999;
    insert_at_cursor(text, cursor, "X");
    EXPECT_EQ(text, "hiX");
    EXPECT_EQ(cursor, 3u);
}

// 场景:"abc" / cursor=3 按一次 Backspace → "ab" / cursor=2(单字节字符)。
TEST(TextInputOpsBackspaceTest, BackspaceSingleByte) {
    std::string text = "abc";
    std::size_t cursor = 3;
    backspace_utf8(text, cursor);
    EXPECT_EQ(text, "ab");
    EXPECT_EQ(cursor, 2u);
}

// 场景:"中"(3 字节) / cursor=3 按一次 Backspace → "" / cursor=0,
// 验证一次退格吞掉整个多字节 glyph 而不是只吞一个 continuation byte。
TEST(TextInputOpsBackspaceTest, BackspaceMultiByteGlyph) {
    std::string text = "\xE4\xB8\xAD"; // "中" (3 bytes)
    std::size_t cursor = 3;
    backspace_utf8(text, cursor);
    EXPECT_EQ(text, "");
    EXPECT_EQ(cursor, 0u);
}

// 场景:空串 + cursor=0 按 Backspace 应该是 no-op,既不崩也不改 state。
TEST(TextInputOpsBackspaceTest, BackspaceOnEmptyBufferIsNoop) {
    std::string text;
    std::size_t cursor = 0;
    backspace_utf8(text, cursor);
    EXPECT_EQ(text, "");
    EXPECT_EQ(cursor, 0u);
}

// 场景:非空串但 cursor=0 按 Backspace 也应是 no-op(没有可回退的字符)。
TEST(TextInputOpsBackspaceTest, BackspaceAtCursorZeroIsNoop) {
    std::string text = "abc";
    std::size_t cursor = 0;
    backspace_utf8(text, cursor);
    EXPECT_EQ(text, "abc");
    EXPECT_EQ(cursor, 0u);
}

// 场景:Delete 在 "abc" / cursor=1 位置 → 删掉 'b',得到 "ac" / cursor=1
//       (cursor 不动,光标仍在原位置但后面少了一格)。
TEST(TextInputOpsDeleteTest, DeleteSingleByte) {
    std::string text = "abc";
    std::size_t cursor = 1;
    delete_utf8(text, cursor);
    EXPECT_EQ(text, "ac");
    EXPECT_EQ(cursor, 1u);
}

// 场景:Delete 在 "a中b"(5 字节)/ cursor=1 位置 → 删掉多字节 "中",
//       得到 "ab" / cursor=1。
TEST(TextInputOpsDeleteTest, DeleteMultiByteGlyph) {
    std::string text = "a\xE4\xB8\xAD" "b"; // "a中b"
    std::size_t cursor = 1;
    delete_utf8(text, cursor);
    EXPECT_EQ(text, "ab");
    EXPECT_EQ(cursor, 1u);
}

// 场景:cursor 已在末尾时按 Delete 是 no-op。
TEST(TextInputOpsDeleteTest, DeleteAtEndIsNoop) {
    std::string text = "abc";
    std::size_t cursor = 3;
    delete_utf8(text, cursor);
    EXPECT_EQ(text, "abc");
    EXPECT_EQ(cursor, 3u);
}

// 场景:"a中"(4 字节)/ cursor=4 按 ArrowLeft → cursor=1
//       (跳过 "中" 的 2 个 continuation byte 到 UTF-8 首字节)。
TEST(TextInputOpsArrowLeftTest, MovesLeftAcrossMultiByteBoundary) {
    std::string text = "a\xE4\xB8\xAD"; // "a中"
    std::size_t cursor = 4;
    move_cursor_left_utf8(text, cursor);
    EXPECT_EQ(cursor, 1u);
}

// 场景:"a中"(4 字节)/ cursor=1 按 ArrowRight → cursor=4
//       (对称前进:跳过 "中" 的首字节和 continuation bytes)。
TEST(TextInputOpsArrowRightTest, MovesRightAcrossMultiByteBoundary) {
    std::string text = "a\xE4\xB8\xAD"; // "a中"
    std::size_t cursor = 1;
    move_cursor_right_utf8(text, cursor);
    EXPECT_EQ(cursor, 4u);
}

// 场景:cursor 越界时,方向键函数 MUST 先 clamp 到 text.size(),再按
// glyph 移动或 no-op,不能崩也不能越读。
TEST(TextInputOpsArrowTest, ClampsOutOfRangeCursor) {
    std::string text = "abc";
    std::size_t cursor = 999;
    move_cursor_left_utf8(text, cursor);
    // clamp 到 3,然后往左一格 → 2
    EXPECT_EQ(cursor, 2u);

    cursor = 999;
    move_cursor_right_utf8(text, cursor);
    // clamp 到 3(末尾),ArrowRight no-op → 仍为 3
    EXPECT_EQ(cursor, 3u);
}
