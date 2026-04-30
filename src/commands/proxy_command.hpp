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
    std::string ca_bundle;     // 空 = 未配置
    bool insecure = false;
};
std::string format_proxy_display(const ProxyDisplaySnapshot& snap);

} // namespace acecode
