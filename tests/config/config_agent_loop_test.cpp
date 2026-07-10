// 覆盖 src/config/config.{hpp,cpp} 中 agent_loop 段的序列化与 clamp 语义,
// 以 align-loop-with-hermes 之后的极简 schema 为基线:
//   - AgentLoopConfig 只剩 max_iterations 一个字段,默认 0(无限制)
//   - max_iterations = 0 表示无限制,正数硬上限钳制到 [1, 10000]
//   - 缺省整段 / 缺省字段 / 不认识的 legacy 字段(auto_continue /
//     max_consecutive_empty_iterations) 都不报错,合规向后兼容
//
// 跟既有 config 测试一致:复刻 load_config() 中相关分支而不依赖真实 config.json,
// 以便在 CI 上稳定跑,不污染用户 ~/.acecode/ 目录。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace acecode;

namespace {

// 模拟 config.cpp 中 load_config() 对 agent_loop 段的解析 + clamp 逻辑,
// 与 align-loop-with-hermes 之后的实现保持一致:不读 auto_continue /
// max_consecutive_empty_iterations,这些 key 出现也不该影响 max_iterations。
int apply_agent_loop_section(const nlohmann::json& alj, AgentLoopConfig& out) {
    int warnings = 0;
    if (alj.contains("max_iterations") && alj["max_iterations"].is_number_integer()) {
        int v = alj["max_iterations"].get<int>();
        if (v < 0) { v = 0; ++warnings; }
        else if (v > 10000) { v = 10000; ++warnings; }
        out.max_iterations = v;
    }
    // add-ask-question-policy:question_policy / question_timeout_seconds
    // 的解析,与 config.cpp 实现保持一致。
    if (alj.contains("question_policy") && alj["question_policy"].is_string()) {
        std::string qp = alj["question_policy"].get<std::string>();
        if (qp == "ask" || qp == "deny" || qp == "timeout") {
            out.question_policy = qp;
            out.question_policy_explicit = true;
        } else {
            ++warnings;
            out.question_policy = "ask";
        }
    }
    if (alj.contains("question_timeout_seconds") &&
        alj["question_timeout_seconds"].is_number_integer()) {
        int v = alj["question_timeout_seconds"].get<int>();
        if (v < 5 || v > 3600) { v = 60; ++warnings; }
        out.question_timeout_seconds = v;
    }
    return warnings;
}

} // namespace

// 场景:AgentLoopConfig 结构体默认 max_iterations=0,表示无限制
TEST(ConfigAgentLoopDefaults, StructDefaults) {
    AgentLoopConfig al;
    EXPECT_EQ(al.max_iterations, 0);
}

// 场景:整个 AppConfig 里 agent_loop.max_iterations 默认 0
TEST(ConfigAgentLoopDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_EQ(cfg.agent_loop.max_iterations, 0);
}

// 场景:config.json 没有 agent_loop 段 → 结构体保持默认值
TEST(ConfigAgentLoopLoader, MissingBlockKeepsDefaults) {
    AppConfig cfg;
    nlohmann::json j = nlohmann::json::object();
    EXPECT_FALSE(j.contains("agent_loop"));
    EXPECT_EQ(cfg.agent_loop.max_iterations, 0);
}

// 场景:agent_loop 段为空对象 → max_iterations 仍是默认值,无警告
TEST(ConfigAgentLoopLoader, EmptyBlockKeepsDefault) {
    AgentLoopConfig al;
    nlohmann::json alj = nlohmann::json::object();
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
    EXPECT_EQ(al.max_iterations, 0);
}

// 场景:max_iterations 在合法范围内 → 直接接受,无警告
TEST(ConfigAgentLoopLoader, AcceptInRangeValue) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", 100}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
    EXPECT_EQ(al.max_iterations, 100);
}

// 场景:max_iterations = 0 表示无限制,直接接受
TEST(ConfigAgentLoopLoader, AcceptUnlimitedZero) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", 0}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
    EXPECT_EQ(al.max_iterations, 0);
}

// 场景:max_iterations < 0 被钳制到 0
TEST(ConfigAgentLoopLoader, ClampMaxIterationsNegative) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", -999}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
    EXPECT_EQ(al.max_iterations, 0);
}

// 场景:max_iterations 超上限 10000 被钳制
TEST(ConfigAgentLoopLoader, ClampMaxIterationsTooLarge) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", 99999}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
    EXPECT_EQ(al.max_iterations, 10000);
}

// 场景:合法边界值(恰好在上下界)原样保留,不触发警告
TEST(ConfigAgentLoopLoader, AcceptBoundaryValues) {
    AgentLoopConfig al0;
    nlohmann::json alj0 = {{"max_iterations", 0}};
    EXPECT_EQ(apply_agent_loop_section(alj0, al0), 0);
    EXPECT_EQ(al0.max_iterations, 0);

    AgentLoopConfig al1;
    nlohmann::json alj1 = {{"max_iterations", 1}};
    EXPECT_EQ(apply_agent_loop_section(alj1, al1), 0);
    EXPECT_EQ(al1.max_iterations, 1);

    AgentLoopConfig al2;
    nlohmann::json alj2 = {{"max_iterations", 10000}};
    EXPECT_EQ(apply_agent_loop_section(alj2, al2), 0);
    EXPECT_EQ(al2.max_iterations, 10000);
}

// 场景:legacy 配置(agentic-loop-terminator 时期写入)里的
// auto_continue / max_consecutive_empty_iterations 字段必须被静默忽略,
// 不影响 max_iterations 的解析,也不报错。
TEST(ConfigAgentLoopLoader, LegacyKeysIgnored) {
    AgentLoopConfig al;
    nlohmann::json alj = {
        {"auto_continue", true},
        {"max_consecutive_empty_iterations", 5},
        {"max_iterations", 60}
    };
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);  // 无警告
    EXPECT_EQ(al.max_iterations, 60);                  // 唯一被读取的字段
    // legacy 字段不应该出现在 AgentLoopConfig 上 —— 编译期约束,不需要运行期断言
}

// ---- add-ask-question-policy: question_policy / question_timeout_seconds ----

// 场景:结构体默认 ask / 60 秒 / 非显式 / 无 CLI 覆盖
TEST(ConfigQuestionPolicyDefaults, StructDefaults) {
    AgentLoopConfig al;
    EXPECT_EQ(al.question_policy, "ask");
    EXPECT_EQ(al.question_timeout_seconds, 60);
    EXPECT_FALSE(al.question_policy_explicit);
    EXPECT_TRUE(al.question_policy_cli.empty());
    EXPECT_EQ(al.question_timeout_seconds_cli, 0);
}

// 场景:配置缺键 → 保持默认且 explicit 标记不置位
TEST(ConfigQuestionPolicyLoader, MissingKeysKeepDefaults) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", 5}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
    EXPECT_EQ(al.question_policy, "ask");
    EXPECT_FALSE(al.question_policy_explicit);
}

// 场景:三个合法值都被接受且置 explicit 标记(含与默认值相同的 "ask")
TEST(ConfigQuestionPolicyLoader, AcceptsValidValuesAndMarksExplicit) {
    for (const char* v : {"ask", "deny", "timeout"}) {
        AgentLoopConfig al;
        nlohmann::json alj = {{"question_policy", v}};
        EXPECT_EQ(apply_agent_loop_section(alj, al), 0) << v;
        EXPECT_EQ(al.question_policy, v);
        EXPECT_TRUE(al.question_policy_explicit) << v;
    }
}

// 场景:非法策略值归一化为 ask、告警、不置 explicit 标记
TEST(ConfigQuestionPolicyLoader, InvalidValueNormalizedToAsk) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"question_policy", "banana"}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
    EXPECT_EQ(al.question_policy, "ask");
    EXPECT_FALSE(al.question_policy_explicit);
}

// 场景:超时秒数越界(两侧)归位默认 60 并告警;边界值 5/3600 原样接受
TEST(ConfigQuestionPolicyLoader, TimeoutSecondsClamped) {
    {
        AgentLoopConfig al;
        nlohmann::json alj = {{"question_timeout_seconds", 2}};
        EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
        EXPECT_EQ(al.question_timeout_seconds, 60);
    }
    {
        AgentLoopConfig al;
        nlohmann::json alj = {{"question_timeout_seconds", 4000}};
        EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
        EXPECT_EQ(al.question_timeout_seconds, 60);
    }
    {
        AgentLoopConfig al;
        nlohmann::json alj = {{"question_timeout_seconds", 5}};
        EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
        EXPECT_EQ(al.question_timeout_seconds, 5);
    }
    {
        AgentLoopConfig al;
        nlohmann::json alj = {{"question_timeout_seconds", 3600}};
        EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
        EXPECT_EQ(al.question_timeout_seconds, 3600);
    }
}

// 场景:默认值不落盘(sparse-on-write)—— 走真实 save_config
TEST(ConfigQuestionPolicySave, DefaultsAreOmitted) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-question-policy-default-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    if (j.contains("agent_loop")) {
        EXPECT_FALSE(j["agent_loop"].contains("question_policy"));
        EXPECT_FALSE(j["agent_loop"].contains("question_timeout_seconds"));
    }
    std::filesystem::remove(path, ec);
}

// 场景:非默认值落盘;explicit 标记与 CLI 覆盖字段永不序列化
TEST(ConfigQuestionPolicySave, PersistsNonDefaultsButNeverRuntimeFields) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-question-policy-save-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.agent_loop.question_policy = "timeout";
    cfg.agent_loop.question_timeout_seconds = 120;
    cfg.agent_loop.question_policy_explicit = true;   // 运行时标记
    cfg.agent_loop.question_policy_cli = "deny";      // CLI 覆盖,不落盘
    cfg.agent_loop.question_timeout_seconds_cli = 30;
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("agent_loop"));
    const auto& alj = j["agent_loop"];
    EXPECT_EQ(alj["question_policy"], "timeout");
    EXPECT_EQ(alj["question_timeout_seconds"], 120);
    EXPECT_FALSE(alj.contains("question_policy_explicit"));
    EXPECT_FALSE(alj.contains("question_policy_cli"));
    EXPECT_FALSE(alj.contains("question_timeout_seconds_cli"));
    std::filesystem::remove(path, ec);
}
