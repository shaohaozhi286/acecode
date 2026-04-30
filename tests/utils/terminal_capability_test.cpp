// 覆盖 src/utils/terminal_capability.cpp 中
// detect_terminal_capabilities_with() 的纯函数行为。所有 case 走依赖注入,
// 不依赖真实 getenv 或 RtlGetVersion,跨平台稳定。
//
// 主要场景:
//   - Cmder/ConEmu(ConEmuPID 命中)→ is_conemu = true,source_label = "Cmder/ConEmu"
//   - Windows Terminal(WT_SESSION 命中)→ is_windows_terminal = true
//   - legacy conhost(build < 17763)→ is_legacy_conhost = true
//   - 多信号同时命中 → 各自独立 + source_label 走优先级
//   - 全空 → 全 false + 空 source_label
//   - version_lookup 返回 nullopt(探测失败 / 非 Windows)→ legacy 保守 false

#include <gtest/gtest.h>

#include "utils/terminal_capability.hpp"

#include <optional>
#include <string>

using namespace acecode;

namespace {

// 工具:用 lambda 构造一个空环境的 env_lookup。
auto make_env_lookup(std::optional<std::string> conemu_pid,
                     std::optional<std::string> wt_session) {
    return [conemu_pid, wt_session](const char* name) -> std::optional<std::string> {
        std::string n(name);
        if (n == "ConEmuPID") return conemu_pid;
        if (n == "WT_SESSION") return wt_session;
        return std::nullopt;
    };
}

auto make_version_lookup(std::optional<unsigned> build) {
    return [build]() -> std::optional<unsigned> { return build; };
}

} // namespace

// 场景:仅 ConEmuPID 命中 → is_conemu=true,source_label="Cmder/ConEmu"
TEST(TerminalCapability, ConEmuOnly) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::string("12345"), std::nullopt),
        make_version_lookup(std::nullopt));
    EXPECT_TRUE(caps.is_conemu);
    EXPECT_FALSE(caps.is_windows_terminal);
    EXPECT_FALSE(caps.is_legacy_conhost);
    EXPECT_EQ(caps.source_label, "Cmder/ConEmu");
}

// 场景:仅 WT_SESSION 命中 → is_windows_terminal=true,source_label 为空
// (WT_SESSION 表示现代终端,不需要提示)
TEST(TerminalCapability, WindowsTerminalOnly) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::nullopt, std::string("guid-string")),
        make_version_lookup(std::nullopt));
    EXPECT_FALSE(caps.is_conemu);
    EXPECT_TRUE(caps.is_windows_terminal);
    EXPECT_FALSE(caps.is_legacy_conhost);
    EXPECT_EQ(caps.source_label, "");
}

// 场景:Windows build 号 17134(Win10 1803,1809 之前)→ legacy 命中
TEST(TerminalCapability, LegacyConhost1803) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::nullopt, std::nullopt),
        make_version_lookup(17134u));
    EXPECT_FALSE(caps.is_conemu);
    EXPECT_FALSE(caps.is_windows_terminal);
    EXPECT_TRUE(caps.is_legacy_conhost);
    EXPECT_EQ(caps.source_label, "legacy Windows console");
}

// 场景:Windows build 号 17763(Win10 1809 起,正好是阈值边界)→ 不命中 legacy
TEST(TerminalCapability, LegacyConhostBoundary1809NotLegacy) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::nullopt, std::nullopt),
        make_version_lookup(17763u));
    EXPECT_FALSE(caps.is_legacy_conhost);
    EXPECT_EQ(caps.source_label, "");
}

// 场景:Windows build 号 19044(Win10 21H2)→ 不命中 legacy
TEST(TerminalCapability, ModernWin10NotLegacy) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::nullopt, std::nullopt),
        make_version_lookup(19044u));
    EXPECT_FALSE(caps.is_legacy_conhost);
}

// 场景:version_lookup 返回 nullopt(非 Windows 或 RtlGetVersion 失败)→
// legacy 保守为 false,不抛异常
TEST(TerminalCapability, NoVersionMeansNotLegacy) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::nullopt, std::nullopt),
        make_version_lookup(std::nullopt));
    EXPECT_FALSE(caps.is_legacy_conhost);
    EXPECT_FALSE(caps.is_conemu);
    EXPECT_FALSE(caps.is_windows_terminal);
    EXPECT_EQ(caps.source_label, "");
}

// 场景:ConEmuPID + WT_SESSION 同时存在(理论不会发生,但要求决策稳定)
// 两个 bool 应该各自独立为 true,source_label 走 ConEmu 优先级
TEST(TerminalCapability, BothConEmuAndWTAreIndependent) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::string("9999"), std::string("xyz")),
        make_version_lookup(std::nullopt));
    EXPECT_TRUE(caps.is_conemu);
    EXPECT_TRUE(caps.is_windows_terminal);
    EXPECT_EQ(caps.source_label, "Cmder/ConEmu");
}

// 场景:ConEmu + 老 conhost 同时命中 → source_label 走 ConEmu
// (ConEmu 是更具体的环境标签,优先于 conhost build 号)
TEST(TerminalCapability, ConEmuOverridesLegacyLabel) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::string("12345"), std::nullopt),
        make_version_lookup(17134u));
    EXPECT_TRUE(caps.is_conemu);
    EXPECT_TRUE(caps.is_legacy_conhost);
    EXPECT_EQ(caps.source_label, "Cmder/ConEmu");
}

// 场景:ConEmuPID 是空字符串 → 不视为命中(env_lookup 仍返回 has_value,
// 但内容为空。环境变量 = "" 这种值通常意味着没启用,保守不命中)
TEST(TerminalCapability, EmptyConEmuPidNotHit) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::string(""), std::nullopt),
        make_version_lookup(std::nullopt));
    EXPECT_FALSE(caps.is_conemu);
    EXPECT_EQ(caps.source_label, "");
}

// 场景:三个信号全空 → 现代终端默认路径
TEST(TerminalCapability, NoSignalsAllFalse) {
    auto caps = detect_terminal_capabilities_with(
        make_env_lookup(std::nullopt, std::nullopt),
        make_version_lookup(std::nullopt));
    EXPECT_FALSE(caps.is_conemu);
    EXPECT_FALSE(caps.is_windows_terminal);
    EXPECT_FALSE(caps.is_legacy_conhost);
    EXPECT_EQ(caps.source_label, "");
}
