#pragma once

#include "../config/saved_models.hpp"
#include "../provider/llm_provider.hpp"
#include "../provider/models_dev_registry.hpp"

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

nlohmann::json hook_registry_source_to_json(const RegistrySource& source);

nlohmann::json build_startup_before_model_load_payload(const std::string& cwd);

nlohmann::json build_startup_models_loaded_payload(
    const std::string& cwd,
    const ModelProfile& active_profile,
    const std::shared_ptr<LlmProvider>& provider);

nlohmann::json build_assistant_message_completed_payload(
    const std::string& cwd,
    const std::string& session_id,
    const std::string& provider_name,
    const std::string& model,
    const ChatMessage& assistant_message);

} // namespace acecode
