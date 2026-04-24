// provider_swap: 主线程切换 effective provider 的 helper。
// 同 provider 类别走 set_model + reconfigure(openai);跨 provider 销毁 + 新建。
// 末尾重算 cfg.context_window。对应 openspec/changes/model-profiles 任务 4.5。
#pragma once

#include "llm_provider.hpp"
#include "../config/config.hpp"
#include "../config/saved_models.hpp"

#include <memory>
#include <mutex>

namespace acecode {

void swap_provider_if_needed(std::shared_ptr<LlmProvider>& handle,
                             std::mutex& mu,
                             const ModelProfile& entry,
                             AppConfig& cfg);

} // namespace acecode
