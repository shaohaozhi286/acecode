#pragma once

// desktop 启动时挑选哪个 workspace 当 active 的纯函数。优先级:
//   1. last_active_workspace_hash(state.json,上次退出时记下的)
//   2. process cwd 的 hash(用户在某项目目录里启动 acecode-desktop.exe)
//   3. registry 第一项(姑且选一个)
//   4. 全空 → 返回空字符串(调用方应渲染 onboarding splash)
//
// 注意: 第 1/2 步选中的 hash 必须**仍存在于 registry**;不在 registry 的 hash
// 视为无效(目录被删 / scan 没扫到),进入下一优先级。

#include "workspace_registry.hpp"

#include <string>

namespace acecode::desktop {

std::string pick_active(const std::string& last_active_hash,
                        const std::string& process_cwd,
                        const WorkspaceRegistry& registry);

} // namespace acecode::desktop
