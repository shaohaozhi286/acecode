// 覆盖 TUI 宽屏侧边栏的文件变更投影逻辑。
// 这些测试只验证 TuiState::Message -> SidebarFileChange 的纯数据聚合,
// 不触碰 FTXUI 渲染。

#include <gtest/gtest.h>

#include "tui/sidebar_model.hpp"

#include <string>
#include <utility>
#include <vector>

using acecode::DiffHunk;
using acecode::ToolSummary;
using acecode::TuiState;
using acecode::tui::collect_sidebar_file_changes;

namespace {

std::vector<DiffHunk> one_hunk() {
    DiffHunk h;
    h.old_start = 1;
    h.old_count = 1;
    h.new_start = 1;
    h.new_count = 1;
    return {h};
}

TuiState::Message changed_message(const std::string& file,
                                  std::string additions,
                                  std::string deletions) {
    TuiState::Message msg;
    msg.role = "tool_result";
    ToolSummary summary;
    summary.object = file;
    if (!additions.empty()) {
        summary.metrics.emplace_back("+", std::move(additions));
    }
    if (!deletions.empty()) {
        summary.metrics.emplace_back("-", std::move(deletions));
    }
    msg.summary = std::move(summary);
    msg.hunks = one_hunk();
    return msg;
}

} // namespace

// 场景:没有结构化 hunks 的 tool_result 不进入 Files Changed。
TEST(SidebarModel, IgnoresMessagesWithoutHunks) {
    TuiState::Message no_hunks = changed_message("src/a.cpp", "3", "1");
    no_hunks.hunks.reset();

    TuiState::Message assistant = changed_message("src/b.cpp", "2", "0");
    assistant.role = "assistant";

    auto changes = collect_sidebar_file_changes({no_hunks, assistant});

    EXPECT_TRUE(changes.empty());
}

// 场景:同一个文件多次编辑时保持首次出现顺序并累计增删行。
TEST(SidebarModel, MergesRepeatedFilesInFirstSeenOrder) {
    auto changes = collect_sidebar_file_changes({
        changed_message("src/a.cpp", "1", "2"),
        changed_message("src/b.cpp", "3", "0"),
        changed_message("src/a.cpp", "4", "1"),
    });

    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].file, "src/a.cpp");
    EXPECT_EQ(changes[0].additions, 5);
    EXPECT_EQ(changes[0].deletions, 3);
    EXPECT_EQ(changes[1].file, "src/b.cpp");
    EXPECT_EQ(changes[1].additions, 3);
    EXPECT_EQ(changes[1].deletions, 0);
}

// 场景:缺失或非数字 metrics 按 0 处理,不污染汇总。
TEST(SidebarModel, MissingOrInvalidMetricsAreZero) {
    auto invalid = changed_message("src/a.cpp", "abc", "  ");
    auto missing = changed_message("src/b.cpp", "", "");

    auto changes = collect_sidebar_file_changes({invalid, missing});

    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].additions, 0);
    EXPECT_EQ(changes[0].deletions, 0);
    EXPECT_EQ(changes[1].additions, 0);
    EXPECT_EQ(changes[1].deletions, 0);
}

// 场景:相对路径和工作区内绝对路径在侧边栏显示为 . 开头的相对路径。
TEST(SidebarModel, WorkspaceFilesDisplayAsDotRelativePaths) {
#ifdef _WIN32
    const std::string cwd = "C:\\work\\repo";
    const std::string relative_file = "src\\relative.cpp";
    const std::string expected_relative = ".\\src\\relative.cpp";
    const std::string absolute_file = "C:\\work\\repo\\src\\a.cpp";
    const std::string expected_absolute = ".\\src\\a.cpp";
#else
    const std::string cwd = "/work/repo";
    const std::string relative_file = "src/relative.cpp";
    const std::string expected_relative = "./src/relative.cpp";
    const std::string absolute_file = "/work/repo/src/a.cpp";
    const std::string expected_absolute = "./src/a.cpp";
#endif

    auto changes = collect_sidebar_file_changes({
        changed_message(relative_file, "1", "0"),
        changed_message(absolute_file, "1", "0"),
    }, cwd);

    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].display_file, expected_relative);
    EXPECT_EQ(changes[1].display_file, expected_absolute);
}

// 场景:工作区外的绝对路径保持全路径显示。
TEST(SidebarModel, ExternalAbsoluteFilesKeepFullPath) {
#ifdef _WIN32
    const std::string cwd = "C:\\work\\repo";
    const std::string external_file = "D:\\outside\\b.cpp";
#else
    const std::string cwd = "/work/repo";
    const std::string external_file = "/outside/b.cpp";
#endif

    auto changes = collect_sidebar_file_changes({
        changed_message(external_file, "1", "0"),
    }, cwd);

    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].display_file, external_file);
}
