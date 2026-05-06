// 覆盖 src/tool/skills_tool.cpp 的 category 过滤鲁棒性:
// - 合法 category 仍保持精确过滤
// - 非法 category(如 * / :) 自动回退到未过滤结果
// - 纯空白 category 视为未提供,不触发 fallback
// - registry 为空时给出 machine-readable reason
// - fallback 路径写出可诊断日志

#include <gtest/gtest.h>

#include "skills/skill_registry.hpp"
#include "tool/skills_tool.hpp"
#include "tool/tool_executor.hpp"
#include "utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace {

void write_skill(const fs::path& root,
                 const std::string& category,
                 const std::string& name,
                 const std::string& description) {
    fs::path dir = root / category / name;
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n";
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool json_array_contains(const nlohmann::json& arr, const std::string& value) {
    for (const auto& item : arr) {
        if (item.is_string() && item.get<std::string>() == value) return true;
    }
    return false;
}

class SkillsToolTest : public ::testing::Test {
protected:
    fs::path temp_root;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        temp_root = fs::temp_directory_path() /
                    ("acecode_skills_tool_test_" + std::to_string(gen()));
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        acecode::Logger::instance().set_level(acecode::LogLevel::Dbg);
        acecode::Logger::instance().init((temp_root / "tearcleaner.log").string());
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }

    void populate_registry_with_two_skills(acecode::SkillRegistry& registry) const {
        fs::path root = temp_root / "skills";
        write_skill(root, "engineering", "alpha", "alpha desc");
        write_skill(root, "research", "beta", "beta desc");

        registry.set_scan_roots({root});
        registry.scan();
    }

    nlohmann::json run_tool(acecode::SkillRegistry& registry,
                            const nlohmann::json& args = nlohmann::json::object()) const {
        auto impl = acecode::create_skills_list_tool(registry);
        acecode::ToolContext ctx;
        auto result = impl.execute(args.dump(), ctx);
        EXPECT_TRUE(result.success);
        return nlohmann::json::parse(result.output);
    }
};

TEST_F(SkillsToolTest, ValidCategoryKeepsExactFiltering) {
    acecode::SkillRegistry registry;
    populate_registry_with_two_skills(registry);

    auto out = run_tool(registry, nlohmann::json{{"category", "engineering"}});

    EXPECT_EQ(out["reason"].get<std::string>(), "ok");
    EXPECT_FALSE(out["fallback_applied"].get<bool>());
    EXPECT_EQ(out["count"].get<int>(), 1);
    ASSERT_EQ(out["skills"].size(), 1u);
    EXPECT_EQ(out["skills"][0]["name"].get<std::string>(), "alpha");
    EXPECT_EQ(out["applied_category"].get<std::string>(), "engineering");
    EXPECT_TRUE(json_array_contains(out["available_categories"], "engineering"));
    EXPECT_TRUE(json_array_contains(out["available_categories"], "research"));
}

TEST_F(SkillsToolTest, InvalidCategoryFallsBackToUnfilteredList) {
    acecode::SkillRegistry registry;
    populate_registry_with_two_skills(registry);

    auto out = run_tool(registry, nlohmann::json{{"category", "*"}});

    EXPECT_EQ(out["reason"].get<std::string>(), "fallback_from_invalid_filter");
    EXPECT_TRUE(out["fallback_applied"].get<bool>());
    EXPECT_EQ(out["invalid_category"].get<std::string>(), "*");
    EXPECT_EQ(out["count"].get<int>(), 2);
    EXPECT_TRUE(json_array_contains(out["categories"], "engineering"));
    EXPECT_TRUE(json_array_contains(out["categories"], "research"));
    EXPECT_NE(out["message"].get<std::string>().find("Ignoring invalid category filter"),
              std::string::npos);
}

TEST_F(SkillsToolTest, WhitespaceOnlyCategoryIsTreatedAsNoFilter) {
    acecode::SkillRegistry registry;
    populate_registry_with_two_skills(registry);

    auto out = run_tool(registry, nlohmann::json{{"category", "   \n\t  "}});

    EXPECT_EQ(out["reason"].get<std::string>(), "ok");
    EXPECT_FALSE(out["fallback_applied"].get<bool>());
    EXPECT_EQ(out["count"].get<int>(), 2);
    EXPECT_FALSE(out.contains("invalid_category"));
    EXPECT_FALSE(out.contains("applied_category"));
}

TEST_F(SkillsToolTest, EmptyRegistryReportsRegistryEmptyReason) {
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root / "empty-skills"});
    registry.scan();

    auto out = run_tool(registry);

    EXPECT_EQ(out["reason"].get<std::string>(), "registry_empty");
    EXPECT_FALSE(out["fallback_applied"].get<bool>());
    EXPECT_EQ(out["count"].get<int>(), 0);
    EXPECT_NE(out["message"].get<std::string>().find("No skills installed"),
              std::string::npos);
}

TEST_F(SkillsToolTest, InvalidCategoryFallbackIsLogged) {
    acecode::SkillRegistry registry;
    populate_registry_with_two_skills(registry);
    fs::path log_path = temp_root / "acecode.log";
    acecode::Logger::instance().init(log_path.string());
    acecode::Logger::instance().set_level(acecode::LogLevel::Dbg);

    auto out = run_tool(registry, nlohmann::json{{"category", ":"}});
    EXPECT_TRUE(out["fallback_applied"].get<bool>());

    std::string body = read_file(log_path);
    EXPECT_NE(body.find("requested_category=':'"), std::string::npos);
    EXPECT_NE(body.find("reason=fallback_from_invalid_filter"), std::string::npos);
    EXPECT_NE(body.find("result_count=2"), std::string::npos);
}

} // namespace