#pragma once

// 终端能力探测 — 给 add-legacy-terminal-fallback 用。
//
// 启动时一次性读环境变量 + Windows 版本号,判断当前终端是否属于以下三类:
//   - Cmder/ConEmu(`ConEmuPID` 环境变量存在)
//   - Windows Terminal(`WT_SESSION` 存在)
//   - 老 Windows console(build < 17763,即 Win10 1809 之前)
//
// 这三个 bool 是独立信号,后续 src/tui/render_mode 的 decide_render_mode()
// 把它们组合到 ScreenRenderMode 决策。
//
// 为了单测,detect_terminal_capabilities_with() 接受可注入的 env / version
// 查询函数;detect_terminal_capabilities() 是用真实 getenv +
// RtlGetVersion 包好的产物函数。

#include <functional>
#include <optional>
#include <string>

namespace acecode {

struct TerminalCapabilities {
    bool is_conemu          = false;
    bool is_windows_terminal = false;
    bool is_legacy_conhost  = false;
    // 给一次性提示用的来源标签,组合优先级:
    //   ConEmu 命中 → "Cmder/ConEmu"
    //   仅 legacy conhost → "legacy Windows console"
    //   其它 → ""
    std::string source_label;
};

// 真实探测:读 getenv("ConEmuPID") / getenv("WT_SESSION") + RtlGetVersion。
TerminalCapabilities detect_terminal_capabilities();

// 测试专用:env_lookup 返回 std::optional<std::string>(nullopt = 未设置);
// version_lookup 返回 std::optional<unsigned> 表示 Windows build 号
// (nullopt = 不在 Windows / 探测失败 / 不应判定为 legacy)。
//
// 这是纯函数,不读任何全局状态,所有数据来自两个回调。
TerminalCapabilities detect_terminal_capabilities_with(
    const std::function<std::optional<std::string>(const char* name)>& env_lookup,
    const std::function<std::optional<unsigned>()>& version_lookup);

} // namespace acecode
