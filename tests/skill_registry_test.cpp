#include "skills/skill_registry.hpp"
#include "agent_loop.hpp"
#include "commands/command_registry.hpp"
#include "config/config.hpp"
#include "permissions.hpp"
#include "tool/tool_executor.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void write_skill(const fs::path& root,
                 const std::string& category,
                 const std::string& name,
                 const std::string& description) {
    fs::path dir = root / acecode::path_from_utf8(category) / acecode::path_from_utf8(name);
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n\n"
        << description << "\n";
}

class NoopProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse response;
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    std::string name() const override { return "noop"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "noop"; }
    void set_model(const std::string&) override {}
};

void append_utf16le(std::string& out, char16_t ch) {
    out.push_back(static_cast<char>(ch & 0xFF));
    out.push_back(static_cast<char>((ch >> 8) & 0xFF));
}

std::string utf16le_bom_bytes(std::u16string_view text) {
    std::string out;
    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xFE));
    for (char16_t ch : text) append_utf16le(out, ch);
    return out;
}

class SkillRegistryCompatTest : public ::testing::Test {
protected:
    fs::path temp_root;

    void SetUp() override {
        temp_root = fs::temp_directory_path() / fs::path("acecode-skill-registry-test");
        std::error_code ec;
        fs::remove_all(temp_root, ec);
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }
};

TEST_F(SkillRegistryCompatTest, LoadsSkillFromAgentRoot) {
    fs::path acecode_root = temp_root / "acecode-root";
    fs::path agent_root = temp_root / "agent-root";
    write_skill(agent_root, "engineering", "agent-only", "loaded from .agent root");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({acecode_root, agent_root});
    registry.scan();

    auto found = registry.find("agent-only");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "agent-only");
    EXPECT_EQ(found->description, "loaded from .agent root");
}

TEST_F(SkillRegistryCompatTest, PrefersEarlierRootForDuplicateSkillNames) {
    fs::path acecode_root = temp_root / "acecode-root";
    fs::path agent_root = temp_root / "agent-root";
    write_skill(acecode_root, "engineering", "shared-skill", "from acecode root");
    write_skill(agent_root, "engineering", "shared-skill", "from agent root");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({acecode_root, agent_root});
    registry.scan();

    auto found = registry.find("shared-skill");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->description, "from acecode root");
    EXPECT_EQ(registry.list().size(), 1u);
}

TEST_F(SkillRegistryCompatTest, PreservesUtf8CategoryDescriptionAndBody) {
    fs::path root = temp_root / "skills-root";
    write_skill(root, u8"中文分类", "unicode-skill", u8"处理中文描述");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();

    auto found = registry.find("unicode-skill");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->category, u8"中文分类");
    EXPECT_EQ(found->description, u8"处理中文描述");
    EXPECT_TRUE(acecode::is_valid_utf8(found->category));
    EXPECT_TRUE(acecode::is_valid_utf8(found->description));

    std::string body = registry.read_skill_body("unicode-skill");
    EXPECT_NE(body.find(u8"处理中文描述"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(body));
}

TEST_F(SkillRegistryCompatTest, LargeUtf8SkillNotGarbledWhenReadBudgetSplitsCharacter) {
    // 触发场景:像 paoffice-im 这类 SKILL.md 把整套 CLI 文档写进 body,文件远超
    // frontmatter 8KB 读取预算。旧实现在第 8192 字节硬切,常把一个 3 字节中文字符
    // 切成两半 → ensure_utf8 判整块非法 UTF-8 → 中文 Windows 上按 GBK 重解码 →
    // 顶部 frontmatter(含 description)从第一个字节起整串乱码。
    // 期望:description 原样保留、仍是合法 UTF-8。
    // 回归 bug 表现:WebUI 斜杠命令列表里该 skill 的描述显示为乱码,其余 skill 正常。
    fs::path root = temp_root / "skills-root";
    fs::path dir = root / "general" / "big-skill";
    fs::create_directories(dir);

    const std::string description = u8"企业 IM 聊天 CLI 工具";
    const std::string header =
        "---\n"
        "name: big-skill\n"
        "description: " + description + "\n"
        "---\n\n";
    // 用 3 字节中文字符 "国"(E5 9B BD)铺满 body,凑到远超 8192 字节。
    std::string cjk_run;
    while (cjk_run.size() < 9000) cjk_run += u8"国";
    // 关键:确定性地让第 8192 字节(首个未读字节)落在续字节上,即读取窗口
    // [0,8191] 必然以残缺字符结尾——这正是旧 bug 的触发条件。用 ASCII 垫片微调对齐。
    std::string pad;
    auto build = [&] { return header + "# big-skill " + pad + "\n\n" + cjk_run; };
    std::string file = build();
    ASSERT_GT(file.size(), 8192u);
    auto is_continuation = [](unsigned char b) { return (b & 0xC0) == 0x80; };
    while (!is_continuation(static_cast<unsigned char>(file[8192]))) {
        pad += "x";
        file = build();
    }

    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs.write(file.data(), static_cast<std::streamsize>(file.size()));
    ofs.close();

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();

    auto found = registry.find("big-skill");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->description, description);
    EXPECT_TRUE(acecode::is_valid_utf8(found->description));
}

TEST_F(SkillRegistryCompatTest, LoadsUtf16LegacyHeaderSkill) {
    fs::path root = temp_root / "skills-root";
    fs::path dir = root / "general" / "calculator";
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << utf16le_bom_bytes(
        u"name: calculator\r\n"
        u"description: A simple calculator skill\r\n"
        u"category: general\r\n"
        u"\r\n"
        u"---\r\n"
        u"\r\n"
        u"# Calculator Skill\r\n");
    ofs.close();

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();

    auto found = registry.find("calculator");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "calculator");
    EXPECT_EQ(found->description, "A simple calculator skill");
    EXPECT_EQ(found->category, "general");
    EXPECT_TRUE(acecode::is_valid_utf8(found->description));

    std::string body = registry.read_skill_body("calculator");
    EXPECT_NE(body.find("# Calculator Skill"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(body));
}

TEST_F(SkillRegistryCompatTest, ListSeesSkillAddedAfterInitialScanWithoutReload) {
    fs::path root = temp_root / "skills-root";

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();
    EXPECT_TRUE(registry.list().empty());

    write_skill(root, "engineering", "fresh-skill", "created after scan");

    auto all = registry.list();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].name, "fresh-skill");
    EXPECT_EQ(all[0].description, "created after scan");
}

TEST_F(SkillRegistryCompatTest, FindSeesUpdatedMetadataWithoutReload) {
    fs::path root = temp_root / "skills-root";
    write_skill(root, "engineering", "mutable-skill", "first description");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();
    ASSERT_EQ(registry.find("mutable-skill")->description, "first description");

    write_skill(root, "engineering", "mutable-skill", "updated description");

    auto updated = registry.find("mutable-skill");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->description, "updated description");
}

TEST_F(SkillRegistryCompatTest, SupportingFilesAreDiscoveredWithoutReload) {
    fs::path root = temp_root / "skills-root";
    write_skill(root, "engineering", "support-skill", "has support");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();
    EXPECT_TRUE(registry.list_supporting_files("support-skill").empty());

    fs::path references = root / "engineering" / "support-skill" / "references";
    fs::create_directories(references);
    std::ofstream ofs(references / "api.md", std::ios::binary);
    ofs << "live reference";
    ofs.close();

    auto files = registry.list_supporting_files("support-skill");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], "references/api.md");
    auto resolved = registry.resolve_skill_file("support-skill", "references/api.md");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(fs::exists(*resolved));
}

TEST_F(SkillRegistryCompatTest, CommandDispatchInvokesNewSkillWithoutReloadingCommands) {
    fs::path root = temp_root / "skills-root";

    acecode::SkillRegistry skill_registry;
    skill_registry.set_scan_roots({root});
    skill_registry.scan();

    acecode::CommandRegistry command_registry;
    acecode::TuiState state;
    state.is_waiting = true;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks callbacks;
    auto provider = std::make_shared<NoopProvider>();
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        callbacks,
        temp_root.string(),
        permissions);
    acecode::AppConfig config;
    acecode::TokenTracker token_tracker;

    acecode::CommandContext ctx{
        state,
        loop,
        nullptr,
        config,
        token_tracker,
        permissions,
        [] {},
        nullptr,
        [] {},
        nullptr,
        &tools,
        &skill_registry,
        nullptr,
        &command_registry,
        temp_root.string(),
    };

    write_skill(root, "engineering", "fresh-skill", "created after command registration");

    EXPECT_TRUE(command_registry.dispatch("/fresh-skill run this", ctx));
    ASSERT_EQ(state.conversation.size(), 1u);
    EXPECT_NE(state.conversation[0].content.find("[Invoking skill: fresh-skill]"),
              std::string::npos);
    ASSERT_EQ(state.pending_queue.size(), 1u);
    EXPECT_NE(state.pending_queue[0].find("Use skill_view(name=\"fresh-skill\")"),
              std::string::npos);
    EXPECT_NE(state.pending_queue[0].find("run this"), std::string::npos);
}

} // namespace
