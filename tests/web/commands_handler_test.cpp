// 覆盖 src/web/handlers/commands_handler.cpp::build_commands_payload。
//
// 该函数是 GET /api/commands 的纯逻辑层。回归点:
//   - builtins 顺序意外被改(前端依赖 init→compact 这个固定顺序展示)
//   - 缺 workspace_cwd 时返回了 skills 字段(应该不返回,向后兼容旧客户端)
//   - 传 workspace_cwd 时漏扫了 .agent/skills 项目目录
//   - skills 没按字典序 → 用户看到的下拉条目跳来跳去
//   - cfg.skills.disabled 中的 skill 漏到响应里
//   - workspace local 与 global 同名 skill 时未走 first-wins(workspace 优先)

#include <gtest/gtest.h>

#include "web/handlers/commands_handler.hpp"

#include "config/config.hpp"
#include "skills/skill_registry.hpp"
#include "utils/encoding.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void write_skill_md(const fs::path& root, const std::string& name,
                     const std::string& description) {
    fs::path dir = root / "general" / name;
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n# " << name << "\n";
}

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

class CommandsHandlerTest : public ::testing::Test {
protected:
    fs::path tmp_root;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        tmp_root = fs::temp_directory_path() /
                   ("acecode_commands_handler_test_" + std::to_string(gen()));
        fs::create_directories(tmp_root);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_root, ec);
    }
};

} // namespace

// 场景:不传 workspace_cwd 时**完全不输出** `skills` 字段(向后兼容)。
// builtins 仍然在,顺序固定 init→compact。
TEST_F(CommandsHandlerTest, NoWorkspaceCwdOmitsSkillsField) {
    acecode::SkillRegistry registry;
    registry.set_scan_roots({tmp_root});
    registry.scan();

    auto payload = acecode::web::build_commands_payload(registry);

    ASSERT_TRUE(payload.contains("builtins"));
    EXPECT_FALSE(payload.contains("skills")) << "缺 workspace_cwd 不应输出 skills 字段";

    ASSERT_EQ(payload["builtins"].size(), 2u);
    EXPECT_EQ(payload["builtins"][0]["name"].get<std::string>(), "init");
    EXPECT_EQ(payload["builtins"][1]["name"].get<std::string>(), "compact");
    EXPECT_FALSE(payload["builtins"][0]["description"].get<std::string>().empty());
    EXPECT_FALSE(payload["builtins"][1]["description"].get<std::string>().empty());
}

// 测试 helper:在 payload.skills 中按 name 找,返回 description(找不到 → nullopt)
static std::optional<std::string> find_skill_desc(const nlohmann::json& payload,
                                                   const std::string& name) {
    if (!payload.contains("skills")) return std::nullopt;
    for (const auto& s : payload["skills"]) {
        if (s["name"].get<std::string>() == name) {
            return s["description"].get<std::string>();
        }
    }
    return std::nullopt;
}

// 场景:传 workspace_cwd + cfg 时,无论是否有 workspace skill,响应中都包含 skills 字段。
// 注意:不假设 skills 数组为空 — 全局 ~/.acecode/skills 可能已有 skills(开发机环境)。
TEST_F(CommandsHandlerTest, WorkspaceCwdYieldsSkillsField) {
    acecode::SkillRegistry global;
    acecode::AppConfig cfg;

    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    ASSERT_TRUE(payload.contains("skills"));
    ASSERT_TRUE(payload["skills"].is_array());
}

// 场景:workspace 内的 skill 出现在响应,且按 name 字典序排列(在我们三个 skill 之间检查相对顺序)。
// 不假设响应里只有这三个,因为全局根可能也有其他 skills。
TEST_F(CommandsHandlerTest, WorkspaceSkillsAppearInSortedOrder) {
    write_skill_md(tmp_root / ".agent" / "skills", "expander-zebra", "z desc");
    write_skill_md(tmp_root / ".agent" / "skills", "expander-alpha", "a desc");
    write_skill_md(tmp_root / ".agent" / "skills", "expander-mango", "m desc");

    acecode::SkillRegistry global;
    acecode::AppConfig cfg;
    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    ASSERT_TRUE(payload.contains("skills"));
    int idx_alpha = -1, idx_mango = -1, idx_zebra = -1;
    for (size_t i = 0; i < payload["skills"].size(); ++i) {
        const std::string n = payload["skills"][i]["name"].get<std::string>();
        if (n == "expander-alpha") idx_alpha = static_cast<int>(i);
        if (n == "expander-mango") idx_mango = static_cast<int>(i);
        if (n == "expander-zebra") idx_zebra = static_cast<int>(i);
    }
    EXPECT_GE(idx_alpha, 0);
    EXPECT_GE(idx_mango, 0);
    EXPECT_GE(idx_zebra, 0);
    EXPECT_LT(idx_alpha, idx_mango);
    EXPECT_LT(idx_mango, idx_zebra);
    EXPECT_EQ(find_skill_desc(payload, "expander-alpha").value_or(""), "a desc");
}

// 场景:cfg.skills.disabled 中的 skill 不出现在响应中。
TEST_F(CommandsHandlerTest, DisabledSkillsAreFiltered) {
    write_skill_md(tmp_root / ".agent" / "skills", "expander-kept", "keep me");
    write_skill_md(tmp_root / ".agent" / "skills", "expander-dropped", "drop me");

    acecode::SkillRegistry global;
    acecode::AppConfig cfg;
    cfg.skills.disabled = {"expander-dropped"};

    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    ASSERT_TRUE(payload.contains("skills"));
    EXPECT_TRUE(find_skill_desc(payload, "expander-kept").has_value());
    EXPECT_FALSE(find_skill_desc(payload, "expander-dropped").has_value())
        << "disabled skill 不应出现在响应里";
}

// 场景:builtins 描述非空,且与 init/compact 命令注册时的描述匹配。
// 防止有人改了 init_command.cpp / builtin_commands.cpp 的描述忘了同步这里。
TEST_F(CommandsHandlerTest, BuiltinDescriptionsMatchTuiRegistration) {
    acecode::SkillRegistry registry;
    registry.scan();

    auto payload = acecode::web::build_commands_payload(registry);

    EXPECT_EQ(payload["builtins"][0]["description"].get<std::string>(),
              "Analyze this codebase and generate (or improve) ACECODE.md");
    EXPECT_EQ(payload["builtins"][1]["description"].get<std::string>(),
              "Compress conversation history");
}

// 场景:workspace_cwd 指向某项目时,该项目 .agent/skills 下的 SKILL.md 出现在响应。
// Desktop 多 workspace 共享 daemon 时,让 workspace 自己的 skill 也能被 GET
// /api/commands 看到的关键路径(global SkillRegistry 只扫 daemon 启动 cwd 链)。
TEST_F(CommandsHandlerTest, WorkspaceCwdScansLocalSkills) {
    write_skill_md(tmp_root / ".agent" / "skills", "expander-ws-only", "ws desc");

    acecode::SkillRegistry global; // 故意不 scan,模拟 daemon 启动 cwd 不在 tmp_root
    acecode::AppConfig cfg;

    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    EXPECT_EQ(find_skill_desc(payload, "expander-ws-only").value_or(""), "ws desc");
}

// 场景:workspace local 与 global 同名 skill,workspace 版本胜出(first-wins)。
// 这是 design.md D2 与 spec 中显式要求的合并语义。
TEST_F(CommandsHandlerTest, WorkspaceLocalSkillWinsOverGlobalSameName) {
    // 第一个 root 是 workspace,写 local 版本
    write_skill_md(tmp_root / ".agent" / "skills", "expander-shared", "from workspace");

    // global registry 用另一个 root 模拟 daemon 启动 cwd 下也有同名 skill
    fs::path global_root = tmp_root / "_other";
    fs::create_directories(global_root);
    write_skill_md(global_root, "expander-shared", "from global");
    acecode::SkillRegistry global;
    global.set_scan_roots({global_root});
    global.scan();

    acecode::AppConfig cfg;
    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    int found_count = 0;
    for (const auto& s : payload["skills"]) {
        if (s["name"].get<std::string>() == "expander-shared") {
            ++found_count;
            EXPECT_EQ(s["description"].get<std::string>(), "from workspace")
                << "workspace 版本应胜出(first-wins)";
        }
    }
    EXPECT_EQ(found_count, 1) << "去重应只剩一条 expander-shared";
}

// 场景:workspace_cwd 是空字符串 → 视为缺省(不输出 skills 字段)
TEST_F(CommandsHandlerTest, EmptyWorkspaceCwdTreatedAsAbsent) {
    acecode::SkillRegistry global;
    acecode::AppConfig cfg;

    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{std::string{}}, &cfg);

    EXPECT_FALSE(payload.contains("skills"));
}

// 场景:本机/global skill 的 frontmatter 可能来自非 UTF-8 文件。命令 API
// 不能因为 description 中有坏字节就在 JSON dump 时 500。
TEST_F(CommandsHandlerTest, InvalidUtf8SkillDescriptionIsSanitized) {
    fs::path dir = tmp_root / ".agent" / "skills" / "general" / "bad-encoding";
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: bad-encoding\n"
        << "description: ";
    ofs.put(static_cast<char>(0xff));
    ofs << "\n---\n\n# bad-encoding\n";
    ofs.close();

    acecode::SkillRegistry global;
    acecode::AppConfig cfg;
    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    auto desc = find_skill_desc(payload, "bad-encoding");
    ASSERT_TRUE(desc.has_value());
    EXPECT_TRUE(acecode::is_valid_utf8(*desc));
    EXPECT_NO_THROW((void)payload.dump());
}

TEST_F(CommandsHandlerTest, Utf16LegacySkillDescriptionIsDecoded) {
    fs::path dir = tmp_root / ".agent" / "skills" / "general" / "calculator";
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

    acecode::SkillRegistry global;
    acecode::AppConfig cfg;
    auto payload = acecode::web::build_commands_payload(
        global, std::optional<std::string>{tmp_root.string()}, &cfg);

    auto desc = find_skill_desc(payload, "calculator");
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(*desc, "A simple calculator skill");
    EXPECT_TRUE(acecode::is_valid_utf8(*desc));
}
