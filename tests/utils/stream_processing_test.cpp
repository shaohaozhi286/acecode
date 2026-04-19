// 覆盖 src/utils/stream_processing.hpp 里三个 bash_tool 流式输出的基础组件:
//   1. strip_ansi     — 把 CSI / OSC 控制序列从 bash 输出里剥掉,只保留可见字符
//   2. utf8_safe_boundary — 在 UTF-8 码点边界处切 chunk,避免半字符漂过 TUI
//   3. feed_line_state    — 按 '\r' / '\n' 维护 tool_progress 的滑动窗口
// 这三者直接决定了用户在工具栏看到的 tail 是不是人类可读、是否有乱码。

#include <gtest/gtest.h>

#include "utils/stream_processing.hpp"

#include <deque>
#include <string>

using acecode::strip_ansi;
using acecode::utf8_safe_boundary;
using acecode::feed_line_state;

// 场景:最常见的 CSI 转义——把着色/重置序列全部去掉,保留纯文本。
TEST(StripAnsi, RemovesCsiSequences) {
    EXPECT_EQ(strip_ansi("\x1b[31mred\x1b[0m"), "red");
    EXPECT_EQ(strip_ansi("plain\x1b[1;32mbright\x1b[mafter"), "plainbrightafter");
}

// 场景:无控制序列的纯文本(含换行、制表)必须原样透传,不做多余改写。
TEST(StripAnsi, PreservesPlainText) {
    EXPECT_EQ(strip_ansi("hello\n"), "hello\n");
    EXPECT_EQ(strip_ansi(""), "");
    EXPECT_EQ(strip_ansi("line1\nline2\tindent"), "line1\nline2\tindent");
}

// 场景:OSC 序列的两种终止方式(ESC \ 与 BEL)都要被正确识别并吞掉;
// 否则终端标题之类的控制数据会漏进 tail。
TEST(StripAnsi, RemovesOscSequences) {
    // OSC 2 ; title (terminated by ESC + backslash)
    EXPECT_EQ(strip_ansi("before\x1b]2;my title\x1b\\after"), "beforeafter");
    // OSC with BEL terminator
    EXPECT_EQ(strip_ansi("x\x1b]0;foo\x07y"), "xy");
}

// 场景:输入长度已经是完整 UTF-8 序列时,返回值应等于输入长度,不做切分。
TEST(Utf8SafeBoundary, ReturnsFullLengthWhenFits) {
    std::string ascii = "hello";
    EXPECT_EQ(utf8_safe_boundary(ascii), ascii.size());
    std::string complete_utf8 = "a" "\xc3\xa9"; // "aé"
    EXPECT_EQ(utf8_safe_boundary(complete_utf8), complete_utf8.size());
}

// 场景:chunk 以不完整多字节序列(0xC3 但少 continuation)结尾,必须在
// 起始字节前切断,让调用者把尾巴拼到下一个 chunk。
TEST(Utf8SafeBoundary, TruncatesBeforeIncompleteMultibyte) {
    std::string incomplete = std::string("a") + "\xc3";
    EXPECT_EQ(utf8_safe_boundary(incomplete), 1u);
}

// 场景:空字符串输入直接返回 0,不能崩。
TEST(Utf8SafeBoundary, HandlesEmptyInput) {
    EXPECT_EQ(utf8_safe_boundary(""), 0u);
}

// 场景:普通换行分隔的输入喂给状态机,每条完整行进入 tail,total 递增,
// 当前行清空。
TEST(FeedLineState, CollectsCompleteLines) {
    std::string cur;
    std::deque<std::string> tail;
    int total = 0;
    feed_line_state("hello\nworld\n", cur, tail, total);
    EXPECT_EQ(total, 2);
    ASSERT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail[0], "hello");
    EXPECT_EQ(tail[1], "world");
    EXPECT_TRUE(cur.empty());
}

// 场景:进度条式的 `\r` 覆盖行为——"10%\r50%\r100%\n" 最终只应计为一条
// 完整行 "100%",而不是三条。
TEST(FeedLineState, CarriageReturnClearsCurrentLine) {
    std::string cur;
    std::deque<std::string> tail;
    int total = 0;
    feed_line_state("10%\r50%\r100%\n", cur, tail, total);
    EXPECT_EQ(total, 1);
    ASSERT_EQ(tail.size(), 1u);
    EXPECT_EQ(tail.back(), "100%");
}
