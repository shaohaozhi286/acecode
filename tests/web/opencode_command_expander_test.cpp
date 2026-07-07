#include <gtest/gtest.h>

#include "web/handlers/opencode_command_expander.hpp"

#include "config/config.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

class OpencodeCommandExpanderTest : public ::testing::Test {
protected:
    fs::path tmp_root;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        tmp_root = fs::temp_directory_path() /
                   ("acecode_opencode_expander_test_" + std::to_string(gen()));
        fs::create_directories(tmp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_root, ec);
    }

    void write_command(const std::string& name, const std::string& body) const {
        fs::path path = tmp_root / ".opencode" / "commands" / (name + ".md");
        fs::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary);
        ofs << "---\n"
            << "description: Test command\n"
            << "---\n"
            << body << "\n";
    }
};

} // namespace

TEST_F(OpencodeCommandExpanderTest, ExpandsWorkspaceCommand) {
    write_command("review", "Review $ARGUMENTS");
    acecode::AppConfig cfg;

    auto result = acecode::web::try_expand_opencode_command(
        "/review src/main.cpp",
        cfg,
        tmp_root.string());

    EXPECT_TRUE(result.expanded);
    EXPECT_EQ(result.command_name, "review");
    EXPECT_EQ(result.text, "Review src/main.cpp");
}

TEST_F(OpencodeCommandExpanderTest, LeavesUnknownCommandUntouched) {
    acecode::AppConfig cfg;

    auto result = acecode::web::try_expand_opencode_command(
        "/missing src/main.cpp",
        cfg,
        tmp_root.string());

    EXPECT_FALSE(result.expanded);
    EXPECT_EQ(result.text, "/missing src/main.cpp");
    EXPECT_EQ(result.command_name, "missing");
}

TEST_F(OpencodeCommandExpanderTest, BuiltinsAreReservedEvenIfCommandFileExists) {
    write_command("init", "Not the built in init");
    acecode::AppConfig cfg;

    auto result = acecode::web::try_expand_opencode_command(
        "/init",
        cfg,
        tmp_root.string());

    EXPECT_FALSE(result.expanded);
    EXPECT_EQ(result.text, "/init");
    EXPECT_EQ(result.command_name, "init");
}
