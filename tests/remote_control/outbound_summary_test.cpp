#include <gtest/gtest.h>

#include "remote_control/outbound_summary.hpp"

#include <string>

using acecode::rc::summarize_tool_args;

namespace {

// 中文字符,每个 UTF-8 编码占 3 字节,用于验证截断不切坏多字节序列。
constexpr const char* kHan = "\xE4\xB8\xAD"; // "中"

std::string repeat(const std::string& s, int n) {
    std::string out;
    out.reserve(s.size() * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

} // namespace

// 场景:arguments 同时有 command 和 file_path。期望:command 优先。
TEST(OutboundSummary, CommandFieldTakesPriority) {
    nlohmann::json args = {{"command", "ls -la"}, {"file_path", "/tmp/x"}};
    EXPECT_EQ(summarize_tool_args("bash", args), "ls -la");
}

// 场景:没有 command,只有 file_path 和 url。期望:file_path 优先于 url。
TEST(OutboundSummary, FilePathFallsBackWhenNoCommand) {
    nlohmann::json args = {{"file_path", "/etc/hosts"}, {"url", "https://example.com"}};
    EXPECT_EQ(summarize_tool_args("file_read", args), "/etc/hosts");
}

// 场景:既无 command 也无 file_path,只有 url。期望:取 url。
TEST(OutboundSummary, UrlFallsBackWhenNoCommandOrFilePath) {
    nlohmann::json args = {{"url", "https://example.com/page"}, {"count", 3}};
    EXPECT_EQ(summarize_tool_args("http_fetch", args), "https://example.com/page");
}

// 场景:command/file_path/url 均不存在。期望:取 arguments 里第一个字符串值。
TEST(OutboundSummary, FirstStringFieldFallsBackWhenNoKnownField) {
    nlohmann::json args = {{"pattern", "*.cpp"}, {"limit", 10}};
    EXPECT_EQ(summarize_tool_args("glob", args), "*.cpp");
}

// 场景:command 字段存在但类型不是字符串(如数字)。期望:跳过它,按同样
// 规则继续找(此处无其它字符串字段,回退到紧凑序列化整个 arguments)。
TEST(OutboundSummary, NonStringCommandFieldIsSkipped) {
    nlohmann::json args = {{"command", 42}, {"retries", 3}};
    EXPECT_EQ(summarize_tool_args("weird_tool", args), R"({"command":42,"retries":3})");
}

// 场景:arguments 不是 object(如数组)。期望:紧凑序列化整个 arguments。
TEST(OutboundSummary, NonObjectArgumentsCompactSerialized) {
    nlohmann::json args = nlohmann::json::array({1, 2, 3});
    EXPECT_EQ(summarize_tool_args("weird_tool", args), "[1,2,3]");
}

// 场景:arguments 是 object 但里面没有任何字符串字段。期望:紧凑序列化
// 整个 arguments。
TEST(OutboundSummary, ObjectWithNoStringFieldCompactSerialized) {
    nlohmann::json args = {{"count", 3}, {"enabled", true}};
    EXPECT_EQ(summarize_tool_args("weird_tool", args), R"({"count":3,"enabled":true})");
}

// 场景:摘要超过 80 字节。期望:截断到 80 字节并追加省略号。
TEST(OutboundSummary, TruncatesLongPreviewWithEllipsis) {
    std::string long_cmd(120, 'x');
    nlohmann::json args = {{"command", long_cmd}};
    std::string result = summarize_tool_args("bash", args);
    EXPECT_EQ(result, std::string(80, 'x') + "\xE2\x80\xA6");
}

// 场景:摘要恰好 80 字节。期望:不截断,无省略号。
TEST(OutboundSummary, ExactlyEightyBytesNotTruncated) {
    std::string cmd(80, 'y');
    nlohmann::json args = {{"command", cmd}};
    EXPECT_EQ(summarize_tool_args("bash", args), cmd);
}

// 场景:81 字节,刚好比上限多 1 字节。期望:截断到 80 并追加省略号。
TEST(OutboundSummary, OneByteOverLimitTruncated) {
    std::string cmd(81, 'z');
    nlohmann::json args = {{"command", cmd}};
    EXPECT_EQ(summarize_tool_args("bash", args), std::string(80, 'z') + "\xE2\x80\xA6");
}

// 场景:中文命令超过 80 字节需要截断。80 不是 3 的倍数,直接按字节截到 80
// 会切在第 27 个字符中间;期望回退到最近的完整字符边界 —— 保留 26 个
// 完整的"中"(78 字节)再加省略号,而不是产出半个 UTF-8 字符。
TEST(OutboundSummary, TruncatesAtUtf8CharBoundaryForChineseText) {
    std::string cmd = repeat(kHan, 30); // 90 字节,30 个字符
    nlohmann::json args = {{"command", cmd}};
    std::string expected = repeat(kHan, 26) + "\xE2\x80\xA6"; // 78 字节 + 省略号
    EXPECT_EQ(summarize_tool_args("bash", args), expected);
}
