#include "terminal_capability.hpp"

#include "logger.hpp"

#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace acecode {

namespace {

// Win10 1809 = build 17763. 这是 conhost 真正修复 VT cursor-up 序列、
// 引入 disable_newline_auto_return 等 ENABLE_VIRTUAL_TERMINAL 行为的版本。
// build < 17763 视为 legacy。
constexpr unsigned kWin10_1809_Build = 17763;

#ifdef _WIN32
// 用 RtlGetVersion 拿到真实 Windows build 号,绕开 GetVersionExW 的兼容性
// shim(那个会一直返回 6.2 即使在 Win11 上)。失败 → nullopt + LOG_WARN。
std::optional<unsigned> probe_windows_build() {
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (!h) {
        LOG_WARN("[terminal_capability] GetModuleHandleW(ntdll.dll) returned NULL");
        return std::nullopt;
    }
    using RtlGetVersionPtr = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    auto fn = reinterpret_cast<RtlGetVersionPtr>(
        reinterpret_cast<void*>(GetProcAddress(h, "RtlGetVersion")));
    if (!fn) {
        LOG_WARN("[terminal_capability] GetProcAddress(RtlGetVersion) returned NULL");
        return std::nullopt;
    }
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) {
        LOG_WARN("[terminal_capability] RtlGetVersion call failed");
        return std::nullopt;
    }
    return static_cast<unsigned>(info.dwBuildNumber);
}
#else
std::optional<unsigned> probe_windows_build() {
    return std::nullopt; // 非 Windows 平台没有 build 号
}
#endif

std::optional<std::string> default_env_lookup(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
}

} // namespace

TerminalCapabilities detect_terminal_capabilities_with(
    const std::function<std::optional<std::string>(const char* name)>& env_lookup,
    const std::function<std::optional<unsigned>()>& version_lookup) {
    TerminalCapabilities caps;

    // ConEmu/Cmder: 任何非空值都视为命中(ConEmu 设置的是 PID 字符串)。
    auto conemu = env_lookup("ConEmuPID");
    if (conemu.has_value() && !conemu->empty()) {
        caps.is_conemu = true;
    }

    // Windows Terminal: WT_SESSION 是 GUID 字符串。同样任何非空都视为命中。
    auto wt = env_lookup("WT_SESSION");
    if (wt.has_value() && !wt->empty()) {
        caps.is_windows_terminal = true;
    }

    // 老 conhost: 只有拿到 build 号且小于 17763 才标记为 legacy。
    auto build = version_lookup();
    if (build.has_value() && *build < kWin10_1809_Build) {
        caps.is_legacy_conhost = true;
    }

    // 来源标签优先级:ConEmu > legacy conhost > 空。
    if (caps.is_conemu) {
        caps.source_label = "Cmder/ConEmu";
    } else if (caps.is_legacy_conhost) {
        caps.source_label = "legacy Windows console";
    }

    return caps;
}

TerminalCapabilities detect_terminal_capabilities() {
    return detect_terminal_capabilities_with(default_env_lookup, probe_windows_build);
}

} // namespace acecode
