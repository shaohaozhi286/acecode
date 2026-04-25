// 覆盖 src/config/config.{hpp,cpp} 中 agent_loop 段的序列化与 clamp 语义,
// 以 align-loop-with-hermes 之后的极简 schema 为基线:
//   - AgentLoopConfig 只剩 max_iterations 一个字段,默认 50
//   - max_iterations 越界时钳制到 [1, 10000]
//   - 缺省整段 / 缺省字段 / 不认识的 legacy 字段(auto_continue /
//     max_consecutive_empty_iterations) 都不报错,合规向后兼容
//
// 跟既有 config 测试一致:复刻 load_config() 中相关分支而不依赖真实 config.json,
// 以便在 CI 上稳定跑,不污染用户 ~/.acecode/ 目录。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 模拟 config.cpp 中 load_config() 对 agent_loop 段的解析 + clamp 逻辑,
// 与 align-loop-with-hermes 之后的实现保持一致:不读 auto_continue /
// max_consecutive_empty_iterations,这些 key 出现也不该影响 max_iterations。
int apply_agent_loop_section(const nlohmann::json& alj, AgentLoopConfig& out) {
    int warnings = 0;
    if (alj.contains("max_iterations") && alj["max_iterations"].is_number_integer()) {
        int v = alj["max_iterations"].get<int>();
        if (v < 1) { v = 1; ++warnings; }
        else if (v > 10000) { v = 10000; ++warnings; }
        out.max_iterations = v;
    }
    return warnings;
}

} // namespace

// 场景:AgentLoopConfig 结构体默认 max_iterations=50
TEST(ConfigAgentLoopDefaults, StructDefaults) {
    AgentLoopConfig al;
    EXPECT_EQ(al.max_iterations, 50);
}

// 场景:整个 AppConfig 里 agent_loop.max_iterations 默认 50
TEST(ConfigAgentLoopDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_EQ(cfg.agent_loop.max_iterations, 50);
}

// 场景:config.json 没有 agent_loop 段 → 结构体保持默认值
TEST(ConfigAgentLoopLoader, MissingBlockKeepsDefaults) {
    AppConfig cfg;
    nlohmann::json j = nlohmann::json::object();
    EXPECT_FALSE(j.contains("agent_loop"));
    EXPECT_EQ(cfg.agent_loop.max_iterations, 50);
}

// 场景:agent_loop 段为空对象 → max_iterations 仍是默认值,无警告
TEST(ConfigAgentLoopLoader, EmptyBlockKeepsDefault) {
    AgentLoopConfig al;
    nlohmann::json alj = nlohmann::json::object();
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
    EXPECT_EQ(al.max_iterations, 50);
}

// 场景:max_iterations 在合法范围内 → 直接接受,无警告
TEST(ConfigAgentLoopLoader, AcceptInRangeValue) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", 100}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 0);
    EXPECT_EQ(al.max_iterations, 100);
}

// 场景:max_iterations = 0 被钳制到 1,产生一条警告
TEST(ConfigAgentLoopLoader, ClampMaxIterationsZero) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", 0}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
    EXPECT_EQ(al.max_iterations, 1);
}

// 场景:max_iterations < 0 被钳制到 1
TEST(ConfigAgentLoopLoader, ClampMaxIterationsNegative) {
    AgentLoopConfig al;
    nlohmann::json alj = {{"max_iterations", -999}};
    EXPECT_EQ(apply_agent_loop_section(alj, al), 1);
    EXPECT_EQ(al.max_iterations, 1);
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
