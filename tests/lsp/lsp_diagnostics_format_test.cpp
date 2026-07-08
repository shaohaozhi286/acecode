// 覆盖 src/lsp/lsp_diagnostics.{hpp,cpp} 的纯格式化函数 +
// src/commands/lsp_command.cpp 的 format_lsp_status。
//
// 覆盖项:
//   - pretty_diagnostic:0-based → 1-based 行列换算、severity 标签、
//     severity 缺省按 ERROR(LSP 允许省略)
//   - report_block:只透出 ERROR(WARN/INFO/HINT 过滤,避免模型跑偏)、
//     无 ERROR 返回空串、20 条截断 + "... and N more" 收尾
//   - append_diagnostics_block:LSP runtime 未初始化时输出零变化
//     (回归保证:未启用 LSP 的既有 file_edit 行为完全不变)
//   - format_lsp_status:enabled/disabled、connected/broken/not-found 分区

#include <gtest/gtest.h>

#include "commands/lsp_command.hpp"
#include "lsp/lsp_diagnostics.hpp"
#include "lsp/lsp_service.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

nlohmann::json make_diag(int severity, int line0, int char0, const std::string& message) {
    return {
        {"severity", severity},
        {"range", {{"start", {{"line", line0}, {"character", char0}}},
                   {"end", {{"line", line0}, {"character", char0 + 1}}}}},
        {"message", message},
    };
}

} // namespace

// 场景:LSP 的 0-based 位置转成用户/模型熟悉的 1-based 编辑器行列。
TEST(LspDiagnosticsFormat, PrettyConvertsToOneBased) {
    EXPECT_EQ(lsp::pretty_diagnostic(make_diag(1, 0, 0, "boom")), "ERROR [1:1] boom");
    EXPECT_EQ(lsp::pretty_diagnostic(make_diag(2, 9, 4, "meh")), "WARN [10:5] meh");
}

// 场景:severity 缺省(LSP 允许省略)按最高级 ERROR 处理 —— 宁可多报。
TEST(LspDiagnosticsFormat, MissingSeverityTreatedAsError) {
    nlohmann::json diag = {{"message", "no severity"},
                           {"range", {{"start", {{"line", 2}, {"character", 3}}}}}};
    EXPECT_EQ(lsp::pretty_diagnostic(diag), "ERROR [3:4] no severity");
}

// 场景:report_block 只透出 ERROR。WARN/INFO/HINT 噪声会诱导模型去修
// 风格问题,全部过滤;全是非 ERROR → 返回空串(工具输出不加块)。
TEST(LspDiagnosticsFormat, ReportFiltersNonErrors) {
    std::vector<nlohmann::json> diags = {
        make_diag(2, 0, 0, "warn"),
        make_diag(1, 1, 0, "real error"),
        make_diag(3, 2, 0, "info"),
        make_diag(4, 3, 0, "hint"),
    };
    const std::string block = lsp::report_block("src/a.cpp", diags);
    EXPECT_NE(block.find("<diagnostics file=\"src/a.cpp\">"), std::string::npos);
    EXPECT_NE(block.find("ERROR [2:1] real error"), std::string::npos);
    EXPECT_EQ(block.find("warn"), std::string::npos);
    EXPECT_EQ(block.find("info"), std::string::npos);
    EXPECT_EQ(block.find("hint"), std::string::npos);

    std::vector<nlohmann::json> only_warns = {make_diag(2, 0, 0, "warn")};
    EXPECT_TRUE(lsp::report_block("src/a.cpp", only_warns).empty());
    EXPECT_TRUE(lsp::report_block("src/a.cpp", {}).empty());
}

// 场景:超过 20 条(kMaxDiagnosticsPerFile,防单文件错误刷爆工具输出)
// 截断,以 "... and N more" 收尾。
TEST(LspDiagnosticsFormat, ReportTruncatesAtTwenty) {
    std::vector<nlohmann::json> diags;
    for (int i = 0; i < 25; ++i) diags.push_back(make_diag(1, i, 0, "e" + std::to_string(i)));
    const std::string block = lsp::report_block("a.cpp", diags);
    EXPECT_NE(block.find("e0"), std::string::npos);
    EXPECT_NE(block.find("e19"), std::string::npos);
    EXPECT_EQ(block.find("e20"), std::string::npos);
    EXPECT_NE(block.find("... and 5 more"), std::string::npos);
}

// 场景(回归):LSP runtime 未初始化时,append_diagnostics_block 对输出
// 零改动、零延迟 —— 既有 file_edit/file_write 单测在无 LSP 环境下必须
// 保持原行为。
TEST(LspDiagnosticsFormat, AppendIsNoopWithoutRuntime) {
    lsp::set_service_for_test(nullptr); // 确保单例为空
    std::string output = "Edited a.cpp";
    lsp::append_diagnostics_block(output, "a.cpp", nullptr, "");
    EXPECT_EQ(output, "Edited a.cpp");
}

// 场景:enabled=false 的 service —— append 同样零改动(config 一键全关)。
TEST(LspDiagnosticsFormat, AppendIsNoopWhenDisabled) {
    LspConfig cfg;
    cfg.enabled = false;
    lsp::set_service_for_test(std::make_unique<lsp::LspService>(cfg, "."));
    std::string output = "Edited a.cpp";
    lsp::append_diagnostics_block(output, "a.cpp", nullptr, "");
    EXPECT_EQ(output, "Edited a.cpp");
    lsp::set_service_for_test(nullptr);
}

// 场景:/lsp 状态文本 —— disabled 时给出开启指引;enabled 时分区展示
// connected(含 root 与文件数)/ broken / not-found。
TEST(LspCommandFormat, StatusText) {
    lsp::LspService::Status disabled;
    disabled.enabled = false;
    const std::string off = format_lsp_status(disabled);
    EXPECT_NE(off.find("Enabled    : no"), std::string::npos);
    EXPECT_NE(off.find("\"lsp\""), std::string::npos); // 指引含 config 片段

    lsp::LspService::Status on;
    on.enabled = true;
    on.connected.push_back({"clangd", ".", 3});
    on.broken.push_back({"gopls", "sub/mod", 0});
    on.not_installed.push_back("rust-analyzer");
    const std::string text = format_lsp_status(on);
    EXPECT_NE(text.find("clangd"), std::string::npos);
    EXPECT_NE(text.find("files=3"), std::string::npos);
    EXPECT_NE(text.find("gopls"), std::string::npos);
    EXPECT_NE(text.find("rust-analyzer"), std::string::npos);
    EXPECT_NE(text.find("Not found"), std::string::npos);
}

// 场景:runtime 未初始化时 /lsp 给出明确提示,不 crash。
TEST(LspCommandFormat, DispatchWithoutRuntime) {
    lsp::set_service_for_test(nullptr);
    const std::string text = dispatch_lsp_subcommand("");
    EXPECT_NE(text.find("not initialized"), std::string::npos);
}
