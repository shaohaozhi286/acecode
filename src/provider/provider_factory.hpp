#pragma once

#include "llm_provider.hpp"
#include "../config/saved_models.hpp"
#include <memory>

namespace acecode {

struct AppConfig;

// 基于一个 ModelProfile 构造 provider。对应 openspec/changes/model-profiles
// 任务 4.3 / design.md D4。调用方不需要再持有整份 AppConfig。
// 返回 shared_ptr 以匹配 main.cpp 可替换容器的语义(design D4)。
//
// config(可选):用于设置能力路由上下文(route-attachments-by-capability D5)。
// model_has_vision 始终从 entry.capabilities 推导;config 非空时再据
// has_any_runtime_vision_model(config) 设置 any_vision_model_available,影响非视觉
// 模型剥图后的 fallback 文本措辞。漏传 config 不影响 gate 正确性(只退化措辞)。
std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry,
                                                        const AppConfig* config = nullptr);

} // namespace acecode
