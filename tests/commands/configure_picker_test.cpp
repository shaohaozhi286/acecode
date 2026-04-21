// 覆盖 src/commands/configure_picker.{hpp,cpp} 的纯格式化函数:
// - format_picker_row(label, secondary, width): picker 行文本渲染,所有 call
//   site(FTXUI 路径和 stdin 回落路径)都通过它产出字符串
// - 交互循环(run_ftxui_picker / run_plain_stdin_picker)依赖 FTXUI 和 stdin,
//   不适合单元测试,在 tasks.md 的手动 QA 步骤中覆盖

#include <gtest/gtest.h>

#include "commands/configure_picker.hpp"

#include <string>

using acecode::format_picker_row;

// 场景:secondary 为空时只返回 label 本体,不加两空格 gutter,不加任何尾部空白
TEST(FormatPickerRow, LabelOnly) {
    EXPECT_EQ(format_picker_row("anthropic", "", 80), "anthropic");
}

// 场景:常见情况 —— label + 两空格 gutter + secondary,整体在 width 内
TEST(FormatPickerRow, LabelPlusSecondary) {
    std::string s = format_picker_row("claude-opus-4-7", "ctx=1M  $15/$75", 80);
    // label 必须以 label 开头
    EXPECT_EQ(s.rfind("claude-opus-4-7", 0), 0u);
    // 两空格 gutter 必须夹在中间
    EXPECT_NE(s.find("claude-opus-4-7  ctx=1M"), std::string::npos);
    // secondary 内容必须原样在后
    EXPECT_NE(s.find("$15/$75"), std::string::npos);
    // 小于 width,不应被截断
    EXPECT_LE(s.size(), 80u);
    EXPECT_EQ(s.find("..."), std::string::npos);
}

// 场景:width 不足以完整显示 secondary 时,secondary 被截断并以 "..." 结尾,
// 但 label 永不被裁
TEST(FormatPickerRow, NarrowTruncatesSecondary) {
    // label=10 chars, secondary=40 chars, width=20
    //   10 (label) + 2 (gutter) = 12 → available for secondary = 8 → keep=5 + "..."
    std::string s = format_picker_row("openrouter", std::string(40, 'x'), 20);
    // label 原样保留
    EXPECT_EQ(s.rfind("openrouter", 0), 0u);
    // 以 "..." 结尾
    EXPECT_GE(s.size(), 3u);
    EXPECT_EQ(s.substr(s.size() - 3), "...");
    // 总长度不超过 width
    EXPECT_LE(s.size(), 20u);
}

// 场景:width 小于 label 本身时(病态输入),仍返回完整 label,完全丢弃 secondary,
// 保证用户能看清自己要选的 id
TEST(FormatPickerRow, NarrowPreservesLabel) {
    std::string s = format_picker_row("very-long-provider-id", "extra metadata", 10);
    // label 原样返回
    EXPECT_EQ(s, "very-long-provider-id");
    // secondary 完全消失,也没有 "..."
    EXPECT_EQ(s.find("extra"), std::string::npos);
    EXPECT_EQ(s.find("..."), std::string::npos);
}
