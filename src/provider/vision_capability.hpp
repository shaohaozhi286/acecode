// src/provider/vision_capability.hpp
// 视觉能力判定的共享 helper。序列化层 fallback(openai_provider)与
// vision_analyze 工具(vision_subagent_tool)都依赖这里,确保"是否存在可用
// 视觉模型"的判定口径完全一致(含 is_runtime_model_provider_enabled 过滤)。
// 对应 route-attachments-by-capability design.md D5 与 tasks 1.8。
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"

#include <string>
#include <vector>

namespace acecode {

// 单个 saved model profile 是否带 `vision` 能力标签。
bool model_profile_has_vision(const ModelProfile& profile);

// 返回所有"运行时可用 + 带 vision 标签"的 saved model,并对 openai 条目补齐
// stream_timeout_ms 默认值(与旧 vision_subagent_tool::vision_profiles 行为一致)。
std::vector<ModelProfile> runtime_vision_profiles(const AppConfig& config);

// 系统里是否存在任意"运行时可用 + 带 vision 标签"的 saved model。
bool has_any_runtime_vision_model(const AppConfig& config);

} // namespace acecode
