// 覆盖 src/web/handlers/skill_command_expander.cpp::try_expand_skill_command。
//
// 这个函数是 daemon 端 sendInput 路径上的关键 hook — 把 `/<skill-name> args`
// 形式的 user message rewrite 成 build_activation_message 的输出,与 TUI 行为
// 一致。回归点:
//   - 已知 skill 命中后 expanded=true 且 text 含 activation 模板特征
//   - 未知 skill 透传(不是 builtin 也不是 SKILL.md)
//   - 不以 / 开头透传
//   - args 解析:多空格 / tab / CJK / 空 args
//   - builtin (init/compact) 在 SkillRegistry 里没注册 → 透传(不接管)
//   - 纯 `/` 不会崩(无 head token)
//
// 用 fixture 在 tmp_root/.agent/skills 下写 SKILL.md,然后用
// initialize_skill_registry 构造一个真 registry(因为 read_skill_body 需要文件
// 真存在)。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"
#include "web/handlers/skill_command_expander.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

void write_skill_md(const fs::path& root, const std::string& name,
                     const std::string& description, const std::string& body) {
    fs::path dir = root / "general" / name;
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n" << body << "\n";
}

class SkillCommandExpanderTest : public ::testing::Test {
protected:
    fs::path tmp_root;
    acecode::SkillRegistry registry;
    acecode::AppConfig cfg;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        tmp_root = fs::temp_directory_path() /
                   ("acecode_skill_expander_test_" + std::to_string(gen()));
        fs::create_directories(tmp_root);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_root, ec);
    }

    void scan() {
        acecode::initialize_skill_registry(registry, cfg, tmp_root.string());
    }
};

} // namespace

// 场景:已知 skill `/calculator 33*44` → expanded=true,text 含轻量 invocation 模板特征
// **不**注入 SKILL.md body — 重复调用不会膨胀 context
TEST_F(SkillCommandExpanderTest, KnownSkillExpands) {
    write_skill_md(tmp_root / ".agent" / "skills",
                   "calculator", "Simple calc skill",
                   "## Body section\nThis is the SKILL.md body that should NOT appear in expansion.");
    scan();

    auto r = acecode::web::try_expand_skill_command("/calculator 33*44是多少", registry);
    EXPECT_TRUE(r.expanded);
    EXPECT_EQ(r.skill_name, "calculator");
    // 轻量提示特征:[SYSTEM: ...]、skill 名、description、skill_view 提示、user args
    EXPECT_NE(r.text.find("[SYSTEM:"), std::string::npos);
    EXPECT_NE(r.text.find("calculator"), std::string::npos);
    EXPECT_NE(r.text.find("Simple calc skill"), std::string::npos);
    EXPECT_NE(r.text.find("skill_view"), std::string::npos);
    EXPECT_NE(r.text.find("33*44是多少"), std::string::npos);
    // 关键:SKILL.md body 不应出现在 expanded text
    EXPECT_EQ(r.text.find("This is the SKILL.md body"), std::string::npos)
        << "body 不应被注入 — 让 LLM 用 skill_view 按需加载";
    EXPECT_EQ(r.text.find("## Body section"), std::string::npos);
}

// 场景(回归):重复调用同名 skill,每次产出的轻量提示长度有界,不会随调用次数膨胀
TEST_F(SkillCommandExpanderTest, RepeatedCallsProduceBoundedSizeHint) {
    write_skill_md(tmp_root / ".agent" / "skills",
                   "calculator", "Simple calc",
                   std::string(50000, 'X')); // 50KB body
    scan();

    auto r1 = acecode::web::try_expand_skill_command("/calculator 1+1", registry);
    auto r2 = acecode::web::try_expand_skill_command("/calculator 2+2", registry);
    EXPECT_TRUE(r1.expanded);
    EXPECT_TRUE(r2.expanded);
    // 两次 expansion 的输出都应远小于 50KB body
    EXPECT_LT(r1.text.size(), 1000u) << "轻量提示应当短小;实际 " << r1.text.size();
    EXPECT_LT(r2.text.size(), 1000u);
    // 也不应包含 body 内容
    EXPECT_EQ(r1.text.find(std::string(100, 'X')), std::string::npos);
}

// 场景:未知命令 `/foobar args` → expanded=false,text 透传
TEST_F(SkillCommandExpanderTest, UnknownNamePassThrough) {
    scan();

    auto r = acecode::web::try_expand_skill_command("/foobar 来测试一下", registry);
    EXPECT_FALSE(r.expanded);
    EXPECT_EQ(r.text, "/foobar 来测试一下");
    EXPECT_EQ(r.skill_name, "foobar"); // 解析出来的 head 即使没命中也填,便于日志
}

// 场景:不以 `/` 开头 → expanded=false 透传
TEST_F(SkillCommandExpanderTest, NoLeadingSlashPassThrough) {
    write_skill_md(tmp_root / ".agent" / "skills", "calculator", "x", "y");
    scan();

    auto r = acecode::web::try_expand_skill_command("hello world calculator", registry);
    EXPECT_FALSE(r.expanded);
    EXPECT_EQ(r.text, "hello world calculator");
    EXPECT_TRUE(r.skill_name.empty());
}

// 场景:builtin 名(init/compact)不在 SkillRegistry → 透传(不接管 builtin)
TEST_F(SkillCommandExpanderTest, BuiltinNamesPassThrough) {
    scan();

    auto r1 = acecode::web::try_expand_skill_command("/init", registry);
    EXPECT_FALSE(r1.expanded);
    EXPECT_EQ(r1.text, "/init");

    auto r2 = acecode::web::try_expand_skill_command("/compact 解释下", registry);
    EXPECT_FALSE(r2.expanded);
    EXPECT_EQ(r2.text, "/compact 解释下");
}

// 场景:已知 skill 但无 args(只有命令名) → expanded=true,invocation hint 仍生成
TEST_F(SkillCommandExpanderTest, KnownSkillEmptyArgsExpands) {
    write_skill_md(tmp_root / ".agent" / "skills", "find-skills",
                   "List skills", "Body content not injected.");
    scan();

    auto r = acecode::web::try_expand_skill_command("/find-skills", registry);
    EXPECT_TRUE(r.expanded);
    EXPECT_EQ(r.skill_name, "find-skills");
    EXPECT_NE(r.text.find("List skills"), std::string::npos); // description
    EXPECT_NE(r.text.find("skill_view"), std::string::npos);  // 提示按需加载
    // 无 args 时 "User's request:" 段省略
    EXPECT_EQ(r.text.find("User's request:"), std::string::npos);
}

// 场景:CJK args 与多空白分隔 — UTF-8 不被破坏,头尾空白吃掉
TEST_F(SkillCommandExpanderTest, MultipleWhitespacesAndCjkArgs) {
    write_skill_md(tmp_root / ".agent" / "skills", "translate",
                   "Translate skill", "Body not injected.");
    scan();

    // 多空格分隔 + 中文标点
    auto r = acecode::web::try_expand_skill_command(
        "/translate    把「你好,世界」译成英文", registry);
    EXPECT_TRUE(r.expanded);
    EXPECT_EQ(r.skill_name, "translate");
    // args 应当被 trim 头部空白后传入
    EXPECT_NE(r.text.find("把「你好,世界」译成英文"), std::string::npos);
    EXPECT_EQ(r.text.find("    把"), std::string::npos) << "args 起头多余空白应被吞";
}

// 场景:tab / 换行作分隔 — split_first_token 接受所有 ASCII 空白
TEST_F(SkillCommandExpanderTest, TabAndNewlineAsSeparators) {
    write_skill_md(tmp_root / ".agent" / "skills", "calc", "x", "Use calc.");
    scan();

    auto r1 = acecode::web::try_expand_skill_command("/calc\tquick math", registry);
    EXPECT_TRUE(r1.expanded);
    EXPECT_EQ(r1.skill_name, "calc");

    auto r2 = acecode::web::try_expand_skill_command("/calc\n12+34", registry);
    EXPECT_TRUE(r2.expanded);
    EXPECT_EQ(r2.skill_name, "calc");
}

// 场景:纯 `/`(无 head token)— 不应崩,透传
TEST_F(SkillCommandExpanderTest, BareSlashPassThrough) {
    scan();

    auto r1 = acecode::web::try_expand_skill_command("/", registry);
    EXPECT_FALSE(r1.expanded);
    EXPECT_EQ(r1.text, "/");

    auto r2 = acecode::web::try_expand_skill_command("/   ", registry);
    EXPECT_FALSE(r2.expanded);
    EXPECT_EQ(r2.text, "/   ");
}

// 场景:空 text — 不崩,透传
TEST_F(SkillCommandExpanderTest, EmptyTextPassThrough) {
    scan();

    auto r = acecode::web::try_expand_skill_command("", registry);
    EXPECT_FALSE(r.expanded);
    EXPECT_EQ(r.text, "");
}
