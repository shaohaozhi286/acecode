#pragma once

// /proxy slash command —— 查看 / 临时切换 / 刷新当前生效的 HTTP 代理。
// 实现细节见 openspec/changes/respect-system-proxy/specs/.../spec.md。
//
// 子命令:
//   /proxy                显示当前生效配置 + source
//   /proxy refresh        重新探测系统代理(对 mode=auto 有意义)
//   /proxy off            会话级强制直连(不写 config)
//   /proxy set <url>      会话级切到 manual <url>(不写 config)
//   /proxy reset          回到 config.json 声明的状态

#include "command_registry.hpp"

namespace acecode {

void register_proxy_command(CommandRegistry& registry);

// 暴露纯函数以便单元测试断言文案 —— 不锁状态、不依赖 TUI。
// 输入是从 `proxy_resolver().options_for(target_url)` 拿到的 ProxyOptions
// 的等价快照,以及 `cfg.network`。返回多行字符串,credentials 已脱敏。
struct ProxyDisplaySnapshot {
    std::string effective_url; // 空 = 直连
    std::string source;        // ResolvedProxy::source
    std::string mode;          // cfg.network.proxy_mode

    // openspec/changes/proxy-fallback-on-unreachable: fallback 状态显示。
    // active 为 false → Reachable 行显示 "yes";true → "no (<reason>)" + 多一行
    // "Original proxy : <redacted-url> (<original-source>)"。
    bool reachable = true;             // false = TCP probe 判定原代理不可达
    std::string reachable_reason;      // 仅当 reachable=false 有意义,如 "Refused"
    std::string original_proxy_url;    // 仅当 reachable=false 有意义,已脱敏
    std::string original_proxy_source; // 仅当 reachable=false 有意义,如 "manual"
};
std::string format_proxy_display(const ProxyDisplaySnapshot& snap);

} // namespace acecode
