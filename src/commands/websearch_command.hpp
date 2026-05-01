#pragma once

// /websearch slash 命令 — 查看 / 临时切换 / 刷新联网搜索 backend 与 region。
//
// 子命令:
//   /websearch                显示当前 active backend / region / 缓存时间 / 注册的 backend
//   /websearch refresh        重新探测 region(等价于失效缓存 + 立即探测)
//   /websearch use <backend>  本会话切到指定 backend(不持久化)
//   /websearch reset          回到 cfg + 缓存 region 推导出来的 backend
//
// 详见 openspec/changes/add-web-search-tool/specs/.../spec.md。

#include "command_registry.hpp"

#include <string>

namespace acecode {

void register_websearch_command(CommandRegistry& registry);

// 暴露给单测的纯函数 — 给定状态快照构造给用户看的多行文本(无副作用)。
struct WebSearchDisplaySnapshot {
    std::string active_backend;            // "" = 未激活
    std::string config_backend;            // "auto" / "duckduckgo" / ...
    std::string region;                    // "global" / "cn" / "unknown"
    long long detected_at_ms = 0;          // 0 = 未缓存
    std::vector<std::string> registered;   // 已注册的 backend 名,按字母序
    bool enabled = true;
};
std::string format_websearch_status(const WebSearchDisplaySnapshot& snap);

// 子命令分发(纯字符串路径,便于单测)。返回给用户的多行响应。
// 当 runtime 未初始化时,返回提示文本但不抛。
std::string dispatch_websearch_subcommand(const std::string& sub);

} // namespace acecode
