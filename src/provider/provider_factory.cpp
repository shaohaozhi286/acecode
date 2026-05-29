#include "provider_factory.hpp"
#include "openai_provider.hpp"
#include "copilot_provider.hpp"
#include "vision_capability.hpp"
#include "../config/config.hpp"
#include "../config/model_provider_registry.hpp"
#include "../utils/logger.hpp"

#include <memory>
#include <string>

namespace acecode {

std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry,
                                                        const AppConfig* config) {
    std::shared_ptr<LlmProvider> provider;
    if (entry.provider == "openai") {
        provider = std::make_shared<OpenAiCompatProvider>(
            entry.base_url,
            entry.api_key,
            entry.model,
            entry.stream_timeout_ms.value_or(OpenAiConfig::kDefaultStreamTimeoutMs)
        );
    } else if (entry.provider == "codex") {
        LOG_WARN(std::string("[provider_factory] ") +
                 disabled_model_provider_reason(entry.provider));
        return nullptr;
    } else {
        // 默认走 copilot —— 与旧 create_provider 的 else 分支行为一致。
        provider = std::make_shared<CopilotProvider>(entry.model);
    }

    // 能力路由上下文(route-attachments-by-capability D5)。OpenAI 与 Copilot
    // 都派生自 OpenAiCompatProvider,共用 build_request_body 的图片 gate。
    if (auto compat = std::dynamic_pointer_cast<OpenAiCompatProvider>(provider)) {
        const bool model_has_vision = model_profile_has_vision(entry);
        const bool any_vision = config ? has_any_runtime_vision_model(*config) : false;
        compat->set_vision_routing(model_has_vision, any_vision);
    }
    return provider;
}

} // namespace acecode
