// model_resolver: 纯函数 —— 三级回退解析 effective ModelProfile。
// 对应 openspec/changes/model-profiles 的 Section 2。
// 算法见 design.md D2 与 spec "三级回退解析确定 effective ModelProfile"。
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../session/session_storage.hpp"

#include <optional>
#include <string>

namespace acecode {

// 从 AppConfig 的 legacy 字段(provider / openai / copilot)合成兜底 entry。
// 返回 entry 的 name 固定为 "(legacy)" —— 保留前缀让 picker 区分。
ModelProfile synth_legacy_entry(const AppConfig& cfg);

// 三级回退解析:default(可被 cwd override 替换)→ 若 resume 有 meta 且能
// 匹配到 saved_models 中 (provider, model) 二元组对应的 entry → 采用该 entry;
// 匹配不上时构造 ad-hoc entry(name 以 "(session:" 开头)。找不到任何有效
// entry 时返回 `synth_legacy_entry(cfg)`。
//
// 所有输入显式参数传入,无任何 IO,可进 acecode_testable 单测。
ModelProfile resolve_effective_model(const AppConfig& cfg,
                                   const std::optional<std::string>& cwd_override_name,
                                   const std::optional<SessionMeta>& resumed_meta);

} // namespace acecode
