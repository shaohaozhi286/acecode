// 验证每轮文件变更摘要使用 ToolContext::scratch_dir 识别工作区临时目录，
// 不在展示层重复硬编码 `.acecode/tmp`。

#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

struct ScratchFixture {
    fs::path root;

    ScratchFixture() {
        static std::atomic<int> seq{0};
        root = fs::temp_directory_path() /
            ("acecode_tool_context_scratch_" + std::to_string(++seq));
        fs::create_directories(root);
    }

    ~ScratchFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

acecode::ToolContext make_context(const ScratchFixture& fixture) {
    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(fixture.root);
    ctx.scratch_dir = acecode::path_to_utf8(
        fixture.root / ".acecode" / "tmp" / "session-current");
    return ctx;
}

acecode::ToolResult changed_file_result(const fs::path& path) {
    acecode::ToolResult result{"changed", true};
    acecode::ToolSummary summary;
    summary.object = acecode::path_to_utf8(path);
    result.summary = std::move(summary);
    result.hunks = std::vector<acecode::DiffHunk>{acecode::DiffHunk{}};
    return result;
}

} // namespace

TEST(ToolContextScratchPath, RecognizesWholeManagedTemporaryRoot) {
    ScratchFixture fixture;
    const auto scratch_root =
        fixture.root / ".acecode" / "tmp";
    fs::create_directories(scratch_root / "session-current");
    fs::create_directories(scratch_root / "session-other");

    const auto ctx = make_context(fixture);
    EXPECT_TRUE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        scratch_root / "session-current" / "helper.py")));
    EXPECT_TRUE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        scratch_root / "session-other" / "helper.ps1")));
    EXPECT_TRUE(ctx.is_workspace_scratch_path(
        ".acecode/tmp/session-current/relative.js"));
    EXPECT_FALSE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        fixture.root / ".acecode" / "tmp-backup" / "keep.txt")));
    EXPECT_FALSE(ctx.is_workspace_scratch_path(acecode::path_to_utf8(
        fixture.root / "src" / "main.cpp")));
}

TEST(ToolContextScratchPath, MarksOnlySuccessfulStructuredScratchChanges) {
    ScratchFixture fixture;
    const auto ctx = make_context(fixture);
    const auto scratch_file =
        fixture.root / ".acecode" / "tmp" / "session-current" / "helper.py";

    auto scratch_result = changed_file_result(scratch_file);
    acecode::mark_workspace_scratch_change(scratch_result, ctx);
    EXPECT_TRUE(scratch_result.metadata.value(
        acecode::kExcludeFromTurnChangeSummaryMetadata, false));

    auto source_result = changed_file_result(fixture.root / "src" / "main.cpp");
    acecode::mark_workspace_scratch_change(source_result, ctx);
    EXPECT_FALSE(source_result.metadata.contains(
        acecode::kExcludeFromTurnChangeSummaryMetadata));

    auto failed_result = changed_file_result(scratch_file);
    failed_result.success = false;
    acecode::mark_workspace_scratch_change(failed_result, ctx);
    EXPECT_FALSE(failed_result.metadata.contains(
        acecode::kExcludeFromTurnChangeSummaryMetadata));
}
