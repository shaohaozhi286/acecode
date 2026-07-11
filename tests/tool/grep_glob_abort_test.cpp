// grep / glob 工具的 abort 响应回归测试。
//
// Bug 表现(2026-07-11 daemon 日志复盘):用户在 Web UI 点「停止」,abort_flag
// 已置位、SSE 也断了,但当时正在执行的内置 grep(大仓库 + 慢盘全量递归)完全
// 不检查 abort_flag,阻塞了 35 秒才自己跑完 —— 用户观感是「终止失败,切回
// 会话发现还在跑」。glob 的递归遍历同样裸奔。修复后两个工具在遍历/逐行匹配
// 循环里检查 ctx.abort_flag,置位立即返回 [Aborted](success=false)。
#include <gtest/gtest.h>

#include "tool/glob_tool.hpp"
#include "tool/grep_tool.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path path;

    TempTree() {
        path = fs::temp_directory_path() /
               ("acecode_grep_glob_abort_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

// 造一棵有若干文件的目录树,保证遍历循环至少跑进第一次迭代的 abort 检查。
void populate_tree(const fs::path& root) {
    for (int i = 0; i < 10; ++i) {
        write_file(root / ("dir" + std::to_string(i)) / "file.txt",
                   "needle line\nother line\n");
    }
}

} // namespace

// 触发场景:abort_flag 已置位(用户已点停止),模型仍发起目录级 grep。
// 期望行为:立即返回 [Aborted] 且 success=false,不产出任何匹配结果。
TEST(GrepGlobAbortTest, GrepDirectoryScanReturnsAbortedWhenFlagSet) {
    TempTree tmp;
    populate_tree(tmp.path);

    std::atomic<bool> abort{true};
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(tmp.path);
    ctx.abort_flag = &abort;

    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(R"({"pattern":"needle"})", ctx);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("[Aborted]"), std::string::npos) << result.output;
    // 不应混入正常匹配输出——abort 结果必须是纯粹的终止标记
    EXPECT_EQ(result.output.find("needle line"), std::string::npos) << result.output;
}

// 触发场景:path 指向单个文件(grep 的逐行匹配分支),abort_flag 已置位。
// 期望行为:行循环里的检查生效,同样返回 [Aborted]。目录遍历循环的检查
// 覆盖不到这个分支,必须单独回归。
TEST(GrepGlobAbortTest, GrepSingleFileScanReturnsAbortedWhenFlagSet) {
    TempTree tmp;
    write_file(tmp.path / "single.txt", "needle line\nsecond line\n");

    std::atomic<bool> abort{true};
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(tmp.path);
    ctx.abort_flag = &abort;

    const nlohmann::json args{
        {"pattern", "needle"},
        {"path", acecode::path_to_utf8(tmp.path / "single.txt")},
    };
    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("[Aborted]"), std::string::npos) << result.output;
}

// 触发场景:abort_flag 已置位,模型发起 glob 全仓遍历。
// 期望行为:立即返回 [Aborted] 且 success=false。
TEST(GrepGlobAbortTest, GlobScanReturnsAbortedWhenFlagSet) {
    TempTree tmp;
    populate_tree(tmp.path);

    std::atomic<bool> abort{true};
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(tmp.path);
    ctx.abort_flag = &abort;

    auto tool = acecode::create_glob_tool();
    auto result = tool.execute(R"({"pattern":"**/*.txt"})", ctx);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("[Aborted]"), std::string::npos) << result.output;
}

// 触发场景:abort_flag 非空但从未置位(正常执行路径)。
// 期望行为:两个工具照常返回完整结果——abort 检查不得误伤正常调用。
TEST(GrepGlobAbortTest, UnsetFlagDoesNotAffectNormalResults) {
    TempTree tmp;
    populate_tree(tmp.path);

    std::atomic<bool> abort{false};
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(tmp.path);
    ctx.abort_flag = &abort;

    auto grep_result = acecode::create_grep_tool().execute(R"({"pattern":"needle"})", ctx);
    ASSERT_TRUE(grep_result.success) << grep_result.output;
    EXPECT_NE(grep_result.output.find("needle line"), std::string::npos);

    auto glob_result = acecode::create_glob_tool().execute(R"({"pattern":"**/*.txt"})", ctx);
    ASSERT_TRUE(glob_result.success) << glob_result.output;
    EXPECT_NE(glob_result.output.find("dir0/file.txt"), std::string::npos);
}
