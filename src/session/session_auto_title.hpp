#pragma once

#include "../config/config.hpp"
#include "../provider/llm_provider.hpp"

#include <memory>
#include <optional>
#include <string>

namespace acecode {

std::string visible_auto_title_input(const UserInput& input);

std::optional<ModelProfile> resolve_auto_title_profile(
    const AppConfig& cfg,
    const std::string& session_model_name,
    const std::string& cwd);

std::shared_ptr<LlmProvider> create_auto_title_provider(ModelProfile profile,
                                                        const AppConfig& cfg);

std::optional<std::string> generate_auto_session_title(
    LlmProvider& provider,
    const std::string& visible_text,
    const AppConfig& cfg);

} // namespace acecode
