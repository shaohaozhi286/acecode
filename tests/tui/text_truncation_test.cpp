// 覆盖 src/tui/text_truncation.cpp 的纯字符串截断逻辑。
// 这些测试不触碰 FTXUI 渲染,只验证 summary 中段省略依赖的宽度计算、
// head/tail 预算分配和 UTF-8 codepoint 边界。

#include <gtest/gtest.h>

#include "tui/text_truncation.hpp"

#include <string>

using acecode::tui::truncate_middle;
using acecode::tui::truncate_middle_segment;
using acecode::tui::visual_width;

// 场景:短字符串在预算内时必须原样返回,不能插入省略号。
TEST(TextTruncation, ShortStringReturnsAsIs) {
    EXPECT_EQ(truncate_middle("src/foo.cpp", 80), "src/foo.cpp");
}

// 场景:长 ASCII 路径 max=40 时按 40%/60% 预算保留头 16 字符和尾 23 字符。
TEST(TextTruncation, LongAsciiPathKeepsHeadTailAndBasename) {
    const std::string path = "apps/web/src/features/auth/components/LoginForm.tsx";
    const std::string out = truncate_middle(path, 40);

    EXPECT_EQ(visual_width(out), 40);
    EXPECT_EQ(out, "apps/web/src/fea\xE2\x80\xA6omponents/LoginForm.tsx");
    EXPECT_NE(out.find("LoginForm.tsx"), std::string::npos);
}

// 场景:bash 命令 preview 超宽时被中段截断,保留开头命令和尾部参数片段。
TEST(TextTruncation, BashPreviewIsMiddleTruncated) {
    const std::string command =
        "npm install --save-dev @types/react @types/node ...";
    const std::string out = truncate_middle(command, 30);

    EXPECT_LE(visual_width(out), 30);
    EXPECT_NE(out.find("\xE2\x80\xA6"), std::string::npos);
    EXPECT_TRUE(out.rfind("npm install ", 0) == 0);
}

// 场景:summary 行只允许截断 object,icon/verb/metrics 这些 affix 必须完整保留。
TEST(TextTruncation, SegmentTruncationPreservesAffixes) {
    const std::string prefix = "\xE2\x9C\x8E Read \xC2\xB7 ";
    const std::string object = "apps/web/src/features/auth/components/LoginForm.tsx";
    const std::string suffix = " \xC2\xB7 420 lines";

    const std::string out = truncate_middle_segment(prefix, object, suffix, 60);

    EXPECT_LE(visual_width(out), 60);
    EXPECT_TRUE(out.rfind(prefix, 0) == 0);
    EXPECT_EQ(out.substr(out.size() - suffix.size()), suffix);
    EXPECT_NE(out.find("\xE2\x80\xA6"), std::string::npos);
    EXPECT_NE(out.find("LoginForm.tsx"), std::string::npos);
}

// 场景:CJK 字符按 1 列保守计数,并且截断不能切在 UTF-8 续字节中间。
TEST(TextTruncation, CjkStringKeepsUtf8CodepointBoundaries) {
    const std::string path = "src/\xE5\xB7\xA5\xE5\x85\xB7/"
                             "\xE6\x96\x87\xE4\xBB\xB6"
                             "\xE8\xAF\xBB\xE5\x8F\x96.cpp";
    const std::string out = truncate_middle(path, 12);

    EXPECT_LE(visual_width(out), 12);
    EXPECT_NE(out.find("\xE2\x80\xA6"), std::string::npos);
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if ((c & 0x80) == 0x00) continue;
        size_t len = 0;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else FAIL() << "invalid UTF-8 leading byte";

        ASSERT_LE(i + len, out.size());
        for (size_t j = i + 1; j < i + len; ++j) {
            const unsigned char next = static_cast<unsigned char>(out[j]);
            EXPECT_EQ(next & 0xC0, 0x80);
        }
        i += len - 1;
    }
}

// 场景:max=2/1/0 和空串这些边界不崩溃,且遵循超短预算返回省略号的约定。
TEST(TextTruncation, HandlesTinyBudgetsAndEmptyString) {
    EXPECT_EQ(truncate_middle("abcd", 2), "\xE2\x80\xA6""d");
    EXPECT_EQ(truncate_middle("abcd", 1), "\xE2\x80\xA6");
    EXPECT_EQ(truncate_middle("abcd", 0), "\xE2\x80\xA6");
    EXPECT_EQ(truncate_middle("", 0), "");
}
