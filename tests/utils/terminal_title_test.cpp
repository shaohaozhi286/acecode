// 覆盖 src/utils/terminal_title.cpp 的 sanitize_title:
//   - 合法 ASCII / UTF-8 必须原样通过
//   - 任何 C0 控制字符(含换行/制表)必须被拒绝
//   - 超长输入必须按 UTF-8 边界安全截断,不留半字符
// 这些保证是 /title 命令不会把恶意 ESC 注入终端、也不会写出损坏 UTF-8 的
// 关键前提,本套测试作为它们的回归基线。

#include <gtest/gtest.h>

#include "utils/terminal_title.hpp"
#include "utils/encoding.hpp"

#include <string>

using acecode::sanitize_title;
using acecode::is_valid_utf8;

// 场景:正常的英文 + 中文 + 带重音符的 UTF-8 输入应无修改通过,error_out 为空。
TEST(SanitizeTitle, AcceptsAsciiAndUtf8) {
    std::string s = "fix login bug 中文 é";
    std::string err = "nonempty";
    EXPECT_TRUE(sanitize_title(s, err));
    EXPECT_EQ(s, "fix login bug 中文 é");
    EXPECT_TRUE(err.empty());
}

// 场景:含 ESC / BEL / NUL / LF / TAB / CR 等控制字节的输入全部被拒绝,
// error_out 固定为 "invalid control character"。任何一条漏网就意味着 OSC 2
// 可能被注入额外控制序列,风险极高。
TEST(SanitizeTitle, RejectsControlChars) {
    for (const std::string& bad : {
        std::string("esc\x1b"),
        std::string("bel\x07"),
        std::string("null\x00", 5),
        std::string("line\n"),
        std::string("tab\t"),
        std::string("cr\r"),
    }) {
        std::string copy = bad;
        std::string err;
        EXPECT_FALSE(sanitize_title(copy, err))
            << "should reject input with control chars, hex first="
            << std::hex << (int)(unsigned char)bad.front();
        EXPECT_EQ(err, "invalid control character");
    }
}

// 场景:喂 100 个"中"字共 300 字节,必须被截断到 <= 256 字节且截断点落在
// UTF-8 码点边界(即 3 字节整倍数),结果仍是合法 UTF-8。若截到半字符,
// 终端会渲染出替换字符(U+FFFD),体验破损。
TEST(SanitizeTitle, TruncatesAtUtf8Boundary) {
    std::string s;
    for (int i = 0; i < 100; ++i) s += "中";
    ASSERT_EQ(s.size(), 300u);

    std::string err;
    EXPECT_TRUE(sanitize_title(s, err));
    EXPECT_EQ(err, "truncated");
    EXPECT_LE(s.size(), 256u);
    EXPECT_EQ(s.size() % 3, 0u) << "must stop on 3-byte boundary (每个'中'占3字节)";
    EXPECT_TRUE(is_valid_utf8(s)) << "truncated result must still be valid UTF-8";
}
