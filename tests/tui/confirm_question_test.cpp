// 覆盖 src/tui/confirm_question.cpp 中 build_confirm_question() 的纯函数行为。
// 该函数被 main.cpp 渲染 confirm overlay 时调用,把 (tool_name, args_json)
// 映射成多行结构化的疑问句,例如:
//   "Do you want to write to /path/to/x.txt?\n  3 lines, 42 bytes"
//
// 规则总览:
//   bash       → 第一行 "Do you want to run this command?",
//                第二/三/四行 "  $ <cmd 1>" / "    <cmd 2>" / "    <cmd 3>";
//                超 3 行追加 "    ...";单行命令长度截断阈值 120。
//   file_write → "Do you want to write to <path 截断 80>?";
//                若 content 字段存在,追加 "  N line(s), M byte(s)"。
//   file_edit  → "Do you want to edit <path 截断 80>?";
//                若 old/new 字段都存在,追加 "  - <old>" / "  + <new>"
//                (单行截断到 60 字符;原字符串多行时附加 "↵" 提示)。
//   其他 / 解析失败 / 缺字段 → "Do you want to use <tool_name>?"
//
// 截断阈值与 main.cpp 渲染层耦合:overlay 行宽容得下 80 字符路径但不容
// heredoc,所以命令多行截 3 行、路径 80、单行 old/new 60。

#include <gtest/gtest.h>

#include "tui/confirm_question.hpp"

using acecode::tui::build_confirm_question;

// 场景:bash + 短命令,首行疑问句 + 第二行 "  $ <cmd>" 完整保留
TEST(ConfirmQuestion, BashShortCommandIncludesCommand) {
    std::string s = build_confirm_question("bash", R"({"command":"ls -la"})");
    EXPECT_NE(s.find("Do you want to run this command?"), std::string::npos);
    EXPECT_NE(s.find("  $ ls -la"), std::string::npos);
}

// 场景:bash + 长命令(120+ 字符),应在 120 处尾部截断为 "..."
TEST(ConfirmQuestion, BashLongCommandTruncatedAt120) {
    std::string long_cmd(200, 'a');
    std::string args = R"({"command":")" + long_cmd + R"("})";
    std::string s = build_confirm_question("bash", args);
    // 117 个 a + "..."(共 120) 应出现
    EXPECT_NE(s.find(std::string(117, 'a') + "..."), std::string::npos);
    // 完整 200 字符不应原样落进结果
    EXPECT_EQ(s.find(std::string(200, 'a')), std::string::npos);
}

// 场景:bash + 多行命令(2 行),应分别带前缀 "  $ " 和 "    " 显示
TEST(ConfirmQuestion, BashMultilineCommandKeepsTwoLines) {
    std::string s = build_confirm_question("bash",
        R"({"command":"echo hi\nls -la"})");
    EXPECT_NE(s.find("  $ echo hi"), std::string::npos);
    EXPECT_NE(s.find("    ls -la"), std::string::npos);
    // 只有 2 行,不应有 "..." 溢出标记
    EXPECT_EQ(s.find("    ..."), std::string::npos);
}

// 场景:bash + 5 行命令,只展示前 3 行,末尾追加 "    ..."
TEST(ConfirmQuestion, BashOverflowMultilineShowsEllipsis) {
    std::string s = build_confirm_question("bash",
        R"({"command":"a\nb\nc\nd\ne"})");
    EXPECT_NE(s.find("  $ a"), std::string::npos);
    EXPECT_NE(s.find("    b"), std::string::npos);
    EXPECT_NE(s.find("    c"), std::string::npos);
    EXPECT_NE(s.find("    ..."), std::string::npos);
    // 第 4/5 行(d / e)不应出现
    EXPECT_EQ(s.find("    d"), std::string::npos);
}

// 场景:bash + 缺 command 字段,fallback 仅保留 base 句、不附第二行
TEST(ConfirmQuestion, BashMissingCommandStillReturnsBase) {
    std::string s = build_confirm_question("bash", R"({"timeout":10})");
    EXPECT_NE(s.find("Do you want to run this command?"), std::string::npos);
    EXPECT_EQ(s.find("$ "), std::string::npos);
}

// 场景:file_write + 短路径 + content,标题含完整路径,详情行含字节/行数
TEST(ConfirmQuestion, FileWriteShortPathWithContent) {
    std::string s = build_confirm_question("file_write",
        R"({"file_path":"hello.txt","content":"hi"})");
    // 首行 — 完整路径
    EXPECT_NE(s.find("Do you want to write to hello.txt?"), std::string::npos);
    // 详情行 — content="hi"(2 字节,1 行)
    EXPECT_NE(s.find("1 line"), std::string::npos);
    EXPECT_NE(s.find("2 bytes"), std::string::npos);
}

// 场景:file_write + 多行 content,行数应正确(末行不带 \n 也算 1 行)
TEST(ConfirmQuestion, FileWriteMultilineContentLineCount) {
    std::string s = build_confirm_question("file_write",
        R"({"file_path":"x","content":"a\nb\nc"})");
    EXPECT_NE(s.find("3 lines"), std::string::npos);
}

// 场景:file_write + 缺 content,只显示路径,无详情行
TEST(ConfirmQuestion, FileWriteWithoutContent) {
    std::string s = build_confirm_question("file_write",
        R"({"file_path":"x.txt"})");
    EXPECT_EQ(s, "Do you want to write to x.txt?");
}

// 场景:file_edit + 长路径(>80),头部应被替换为 "..."
TEST(ConfirmQuestion, FileEditLongPathHeadTruncated) {
    // 100 字符路径 → "..." + 后 77 字符
    std::string long_path(100, 'p');
    std::string args = R"({"file_path":")" + long_path + R"("})";
    std::string s = build_confirm_question("file_edit", args);
    EXPECT_NE(s.find("Do you want to edit ..."), std::string::npos);
    // 完整 100 字符不应原样出现
    EXPECT_EQ(s.find(long_path), std::string::npos);
    // 80 字符以内的尾段应保留
    EXPECT_NE(s.find(std::string(77, 'p')), std::string::npos);
}

// 场景:file_edit + 短路径 + old/new,详情两行 - / + 风格
TEST(ConfirmQuestion, FileEditShowsOldAndNew) {
    std::string s = build_confirm_question("file_edit",
        R"({"file_path":"x.txt","old_string":"foo","new_string":"bar"})");
    EXPECT_NE(s.find("Do you want to edit x.txt?"), std::string::npos);
    EXPECT_NE(s.find("  - foo"), std::string::npos);
    EXPECT_NE(s.find("  + bar"), std::string::npos);
}

// 场景:file_edit + 多行 old_string,首行末尾应加 "↵" 暗示还有后续行
TEST(ConfirmQuestion, FileEditMultilineOldStringMarkedWithArrow) {
    std::string s = build_confirm_question("file_edit",
        R"({"file_path":"x","old_string":"line1\nline2","new_string":"r"})");
    EXPECT_NE(s.find("  - line1 \xE2\x86\xB5"), std::string::npos);
    // 第二行原文不应原样落进结果(它被截掉了)
    EXPECT_EQ(s.find("line2"), std::string::npos);
}

// 场景:file_edit + 长 old_string,应在 60 字符处截断
TEST(ConfirmQuestion, FileEditLongOldStringTruncatedAt60) {
    std::string long_str(100, 'x');
    std::string args = R"({"file_path":"f","old_string":")" + long_str +
                       R"(","new_string":"r"})";
    std::string s = build_confirm_question("file_edit", args);
    // 57 个 x + "..." 应出现
    EXPECT_NE(s.find("  - " + std::string(57, 'x') + "..."), std::string::npos);
}

// 场景:file_edit + 缺 old/new,只显示路径,不显示 -/+ 行
TEST(ConfirmQuestion, FileEditWithoutOldNew) {
    std::string s = build_confirm_question("file_edit",
        R"({"file_path":"a.txt"})");
    EXPECT_EQ(s, "Do you want to edit a.txt?");
}

// 场景:未知工具,走通用兜底分支
TEST(ConfirmQuestion, UnknownToolFallback) {
    std::string s = build_confirm_question("memory_write", R"({"name":"x"})");
    EXPECT_EQ(s, "Do you want to use memory_write?");
}

// 场景:坏 JSON 不抛异常,落到通用分支
TEST(ConfirmQuestion, MalformedJsonFallsBackQuietly) {
    std::string s = build_confirm_question("bash", "not a json");
    EXPECT_EQ(s, "Do you want to use bash?");
}

// 场景:空 args 也安全,走通用分支
TEST(ConfirmQuestion, EmptyArgsFallsBackQuietly) {
    std::string s = build_confirm_question("file_write", "");
    EXPECT_EQ(s, "Do you want to use file_write?");
}

// 场景:file_write/file_edit 路径字段类型不对(数字),应走通用分支
TEST(ConfirmQuestion, FilePathWrongTypeFallsBack) {
    std::string s = build_confirm_question("file_edit",
        R"({"file_path":123})");
    EXPECT_EQ(s, "Do you want to use file_edit?");
}
