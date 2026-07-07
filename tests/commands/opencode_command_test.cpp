#include <gtest/gtest.h>

#include "commands/opencode_command.hpp"
#include "config/config.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

class OpencodeCommandTest : public ::testing::Test {
protected:
    fs::path tmp_root;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        tmp_root = fs::temp_directory_path() /
                   ("acecode_opencode_command_test_" + std::to_string(gen()));
        fs::create_directories(tmp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_root, ec);
    }

    static void write_file(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary);
        ofs << content;
    }
};

const acecode::OpencodeCommandInfo* find_command(
    const std::vector<acecode::OpencodeCommandInfo>& commands,
    const std::string& name) {
    for (const auto& command : commands) {
        if (command.name == name) return &command;
    }
    return nullptr;
}

} // namespace

TEST_F(OpencodeCommandTest, LoadsCommandAndCommandsFoldersWithMetadata) {
    const fs::path root = tmp_root / "config";
    write_file(root / "commands" / "review.md",
               "---\n"
               "description: File review\n"
               "agent: reviewer\n"
               "model: anthropic/claude\n"
               "variant: high\n"
               "subtask: true\n"
               "---\n"
               "Review files\n");
    write_file(root / "commands" / "nested" / "docs.md", "Write docs");
    write_file(root / "command" / "empty.md", "");

    auto commands = acecode::load_opencode_commands_from_config_roots({root});

    const auto* review = find_command(commands, "review");
    ASSERT_NE(review, nullptr);
    EXPECT_EQ(review->description, "File review");
    EXPECT_EQ(review->agent, "reviewer");
    EXPECT_EQ(review->model, "anthropic/claude");
    EXPECT_EQ(review->variant, "high");
    EXPECT_TRUE(review->subtask);
    EXPECT_TRUE(review->has_subtask);
    EXPECT_EQ(review->template_text, "Review files");

    const auto* nested = find_command(commands, "nested/docs");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->template_text, "Write docs");

    const auto* empty = find_command(commands, "empty");
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->template_text, "");
}

TEST_F(OpencodeCommandTest, LaterRootsOverrideEarlierRoots) {
    const fs::path global_root = tmp_root / "global";
    const fs::path project_root = tmp_root / "project";
    write_file(global_root / "commands" / "review.md",
               "---\ndescription: global\n---\nGlobal prompt\n");
    write_file(project_root / "commands" / "review.md",
               "---\ndescription: project\n---\nProject prompt\n");

    auto commands = acecode::load_opencode_commands_from_config_roots(
        {global_root, project_root});

    const auto* review = find_command(commands, "review");
    ASSERT_NE(review, nullptr);
    EXPECT_EQ(review->description, "project");
    EXPECT_EQ(review->template_text, "Project prompt");
}

TEST_F(OpencodeCommandTest, LoadsJsonConfigCommandsIncludingColonNames) {
    const fs::path root = tmp_root / "config";
    write_file(root / "opencode.jsonc",
               "{\n"
               "  \"command\": {\n"
               "    // JSONC comment\n"
               "    \"opsx:apply\": {\n"
               "      \"template\": \"Apply $1\",\n"
               "      \"description\": \"Apply OpenSpec change\",\n"
               "      \"subtask\": true\n"
               "    }\n"
               "  }\n"
               "}\n");

    auto commands = acecode::load_opencode_commands_from_config_roots({root});

    const auto* apply = find_command(commands, "opsx:apply");
    ASSERT_NE(apply, nullptr);
    EXPECT_EQ(apply->description, "Apply OpenSpec change");
    EXPECT_EQ(apply->template_text, "Apply $1");
    EXPECT_TRUE(apply->subtask);
}

TEST_F(OpencodeCommandTest, LoadsProjectRootOpencodeJsonBesideDotOpencodeRoot) {
    const fs::path project = tmp_root / "project";
    const fs::path dot_opencode = project / ".opencode";
    write_file(project / "opencode.jsonc",
               "{\n"
               "  \"command\": {\n"
               "    \"opsx:apply\": { \"template\": \"Apply $1\" }\n"
               "  }\n"
               "}\n");

    auto commands = acecode::load_opencode_commands_from_config_roots({dot_opencode});

    const auto* apply = find_command(commands, "opsx:apply");
    ASSERT_NE(apply, nullptr);
    EXPECT_EQ(apply->template_text, "Apply $1");
}

TEST_F(OpencodeCommandTest, MarkdownOverridesJsonCommandInSameRoot) {
    const fs::path root = tmp_root / "config";
    write_file(root / "opencode.json",
               "{\n"
               "  \"commands\": {\n"
               "    \"review\": { \"template\": \"JSON review\", \"description\": \"json\" }\n"
               "  }\n"
               "}\n");
    write_file(root / "commands" / "review.md",
               "---\ndescription: markdown\n---\nMarkdown review\n");

    auto commands = acecode::load_opencode_commands_from_config_roots({root});

    const auto* review = find_command(commands, "review");
    ASSERT_NE(review, nullptr);
    EXPECT_EQ(review->description, "markdown");
    EXPECT_EQ(review->template_text, "Markdown review");
}

TEST_F(OpencodeCommandTest, RespectsReuseOpencodeSwitchForProductionRoots) {
    acecode::AppConfig cfg;
    cfg.skills.reuse_opencode = false;

    auto roots = acecode::opencode_command_config_roots(
        cfg, tmp_root.string());

    EXPECT_TRUE(roots.empty());
}

TEST_F(OpencodeCommandTest, ExpandsArgumentsAndPositionalPlaceholders) {
    acecode::OpencodeCommandInfo command;
    command.name = "create-file";
    command.template_text = "Create $1 in $2 with $3";

    const std::string expanded = acecode::expand_opencode_command_template(
        command,
        "config.json src \"{ \\\"key\\\": \\\"value\\\" }\"");

    EXPECT_EQ(expanded, "Create config.json in src with { \"key\": \"value\" }");
}

TEST_F(OpencodeCommandTest, HighestPositionalPlaceholderConsumesRemainingArgs) {
    acecode::OpencodeCommandInfo command;
    command.name = "apply";
    command.template_text = "Change $1 using $2";

    const std::string expanded = acecode::expand_opencode_command_template(
        command,
        "change-123 --fast --dry-run");

    EXPECT_EQ(expanded, "Change change-123 using --fast --dry-run");
}

TEST_F(OpencodeCommandTest, ArgumentsPlaceholderUsesRawArgs) {
    acecode::OpencodeCommandInfo command;
    command.name = "component";
    command.template_text = "Create $ARGUMENTS";

    const std::string expanded = acecode::expand_opencode_command_template(
        command,
        "Button \"with label\"");

    EXPECT_EQ(expanded, "Create Button \"with label\"");
}

TEST_F(OpencodeCommandTest, AppendsArgsWhenTemplateHasNoPlaceholders) {
    acecode::OpencodeCommandInfo command;
    command.name = "review";
    command.template_text = "Review these files";

    const std::string expanded = acecode::expand_opencode_command_template(
        command,
        "src/main.cpp");

    EXPECT_EQ(expanded, "Review these files\n\nsrc/main.cpp");
}

TEST_F(OpencodeCommandTest, WrapsSubtaskPromptWhenRequested) {
    acecode::OpencodeCommandInfo command;
    command.name = "opsx/apply";
    command.description = "Apply an OpenSpec change";
    command.subtask = true;
    command.has_subtask = true;
    command.template_text = "Apply $1";

    const std::string expanded = acecode::expand_opencode_command(
        command,
        "change-123");

    EXPECT_NE(expanded.find("spawn_subagent"), std::string::npos);
    EXPECT_NE(expanded.find("Command: /opsx/apply"), std::string::npos);
    EXPECT_NE(expanded.find("Apply change-123"), std::string::npos);
}

TEST_F(OpencodeCommandTest, ParsesSlashCommandHeadAndArgs) {
    auto parsed = acecode::parse_opencode_slash_command("/opsx:apply change-123 --fast");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->name, "opsx:apply");
    EXPECT_EQ(parsed->args, "change-123 --fast");

    EXPECT_FALSE(acecode::parse_opencode_slash_command("plain text").has_value());
    EXPECT_FALSE(acecode::parse_opencode_slash_command("/").has_value());
}
