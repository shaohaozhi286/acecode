#include <gtest/gtest.h>

#include "tool/file_edit_tool.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::create_file_edit_tool;
using acecode::create_file_write_tool;

namespace {

fs::path temp_file(const std::string& suffix) {
    return fs::temp_directory_path() /
           ("acecode_checkpoint_hook_" + std::to_string(std::random_device{}()) + suffix);
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(FileCheckpointHook, FileWriteCallsHookOnceBeforeMutation) {
    ToolImpl tool = create_file_write_tool();
    auto path = temp_file(".txt");
    fs::remove(path);

    int calls = 0;
    std::vector<std::string> seen;
    ToolContext ctx;
    ctx.track_file_write_before = [&](const std::string& p) {
        ++calls;
        seen.push_back(p);
        EXPECT_FALSE(fs::exists(path));
    };

    nlohmann::json args = {
        {"file_path", path.string()},
        {"content", "new\n"}
    };
    ToolResult result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], path.string());
    EXPECT_EQ(read_file(path), "new\n");

    fs::remove(path);
}

TEST(FileCheckpointHook, FileEditCallsHookOnceBeforeMutation) {
    ToolImpl tool = create_file_edit_tool();
    auto path = temp_file(".txt");
    write_file(path, "alpha\nbeta\n");

    int calls = 0;
    ToolContext ctx;
    ctx.track_file_write_before = [&](const std::string& p) {
        ++calls;
        EXPECT_EQ(p, path.string());
        EXPECT_EQ(read_file(path), "alpha\nbeta\n");
    };

    nlohmann::json args = {
        {"file_path", path.string()},
        {"old_string", "beta"},
        {"new_string", "gamma"}
    };
    ToolResult result = tool.execute(args.dump(), ctx);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(read_file(path), "alpha\ngamma\n");

    fs::remove(path);
}

TEST(FileCheckpointHook, FileEditValidationFailureDoesNotCallHook) {
    ToolImpl tool = create_file_edit_tool();
    auto path = temp_file(".txt");
    write_file(path, "alpha\n");

    int calls = 0;
    ToolContext ctx;
    ctx.track_file_write_before = [&](const std::string&) {
        ++calls;
    };

    nlohmann::json args = {
        {"file_path", path.string()},
        {"old_string", "missing"},
        {"new_string", "gamma"}
    };
    ToolResult result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(calls, 0);

    fs::remove(path);
}

TEST(FileCheckpointHook, FileWriteValidationFailureDoesNotCallHook) {
    ToolImpl tool = create_file_write_tool();

    int calls = 0;
    ToolContext ctx;
    ctx.track_file_write_before = [&](const std::string&) {
        ++calls;
    };

    nlohmann::json args = {{"content", "new\n"}};
    ToolResult result = tool.execute(args.dump(), ctx);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(calls, 0);
}
