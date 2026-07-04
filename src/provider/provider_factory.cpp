#include "provider_factory.hpp"
#include "anthropic_provider.hpp"
#include "openai_provider.hpp"
#include "copilot_provider.hpp"
#include "vision_capability.hpp"
#include "../config/config.hpp"
#include "../config/model_provider_registry.hpp"
#include "../utils/logger.hpp"

#include <memory>
#include <string>
#include <utility>

namespace acecode {

std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry,
                                                        const AppConfig* config) {
    std::shared_ptr<LlmProvider> provider;
    if (entry.provider == "openai") {
        int stream_timeout_ms = entry.stream_timeout_ms.value_or(
            config ? config->openai.stream_timeout_ms
                   : OpenAiConfig::kDefaultStreamTimeoutMs);
        auto request_headers = entry.request_headers;
        if (request_headers.empty() && config) {
            request_headers = config->openai.request_headers;
        }
        provider = std::make_shared<OpenAiCompatProvider>(
            entry.base_url,
            entry.api_key,
            entry.model,
            stream_timeout_ms,
            std::move(request_headers)
        );
    } else if (entry.provider == "anthropic") {
        int stream_timeout_ms = entry.stream_timeout_ms.value_or(
            config ? config->openai.stream_timeout_ms
                   : OpenAiConfig::kDefaultStreamTimeoutMs);
        provider = std::make_shared<AnthropicProvider>(
            entry.base_url,
            entry.api_key,
            entry.model,
            stream_timeout_ms,
            entry.request_headers
        );
    } else if (entry.provider == "copilot") {
        provider = std::make_shared<CopilotProvider>(entry.model);
    } else if (entry.provider == "codex") {
        LOG_WARN(std::string("[provider_factory] ") +
                 disabled_model_provider_reason(entry.provider));
        return nullptr;
    } else {
        if (entry.provider.empty()) {
            LOG_WARN("[provider_factory] no model provider configured");
        } else {
            LOG_WARN("[provider_factory] unknown model provider '" + entry.provider + "'");
        }
        return nullptr;
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
