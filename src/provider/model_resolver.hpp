// model_resolver: 纯函数 —— 解析 effective ModelProfile。
// 对应 openspec/changes/model-profiles 的 Section 2。
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../session/session_storage.hpp"

#include <optional>
#include <string>

namespace acecode {

// 解析顺序:cwd override → default_model_name → saved_models 首项;
// resume meta 优先按 preset name 或 (provider, model) 匹配 saved_models,
// 匹配不上时构造 ad-hoc entry(name 以 "(session:" 开头)。如果配置仍是
// 旧 schema 且 saved_models 为空,会从 provider/openai/copilot 字段合成
// 一个临时 profile,确保 TUI/daemon/desktop 可以先启动。
//
// 所有输入显式参数传入,无任何 IO,可进 acecode_testable 单测。
ModelProfile resolve_effective_model(const AppConfig& cfg,
                                   const std::optional<std::string>& cwd_override_name,
                                   const std::optional<SessionMeta>& resumed_meta);

} // namespace acecode
