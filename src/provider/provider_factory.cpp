#include "provider_factory.hpp"
#include "openai_provider.hpp"
#include "copilot_provider.hpp"
#include "../config/model_provider_registry.hpp"
#include "../utils/logger.hpp"

#include <string>

namespace acecode {

std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry) {
    if (entry.provider == "openai") {
        return std::make_shared<OpenAiCompatProvider>(
            entry.base_url,
            entry.api_key,
            entry.model
        );
    }
    if (entry.provider == "codex") {
        LOG_WARN(std::string("[provider_factory] ") +
                 disabled_model_provider_reason(entry.provider));
        return nullptr;
    }
    // 默认走 copilot —— 与旧 create_provider 的 else 分支行为一致。
    return std::make_shared<CopilotProvider>(entry.model);
}

} // namespace acecode
